from __future__ import annotations

from importlib.resources import files
from ipaddress import IPv4Network, ip_address, ip_network
from math import ceil
from typing import Annotated, Any, Literal
from urllib.parse import urlsplit

from fastapi import APIRouter, Depends, HTTPException, Path, Query, Request
from fastapi.responses import Response
from pydantic import BaseModel, ConfigDict
from sqlalchemy import Integer, and_, case, cast, func, or_, select

from .database import Database
from .models import (
    Device,
    DeviceHealthReport,
    Feedback,
    Sample,
    StateTransition,
    TelemetryRecord,
)
from .operations import OperationsService
from .schemas import (
    DEVICE_ID_PATTERN,
    CommandOut,
    ConfigPut,
    ConfigResponse,
    ConsoleCommandCreate,
    DeviceHealthPage,
    DeviceReleaseOverview,
    DeviceReleaseSelection,
    DeviceReleaseTargetOut,
    OperationalLogPage,
    PresenceState,
    ReleaseBundleImport,
    ReleaseSummary,
    SampleOut,
    SanitizedCoreDumpPage,
)
from .service import NotFoundError, PresenceService, utc_now_ms

ONLINE_THRESHOLD_MS = 120_000
MAX_EVENT_MARKERS = 2_000
_CONSOLE_LAN_NETWORK = ip_network("192.168.0.0/24")
_ASSET_MEDIA_TYPES = {
    "console.css": "text/css; charset=utf-8",
    "console.js": "text/javascript; charset=utf-8",
}
_SECURITY_HEADERS = {
    "Content-Security-Policy": (
        "default-src 'self'; base-uri 'none'; frame-ancestors 'none'; "
        "form-action 'self'; img-src 'self' data:; object-src 'none'; "
        "script-src 'self'; style-src 'self'; connect-src 'self'"
    ),
    "Referrer-Policy": "no-referrer",
    "X-Content-Type-Options": "nosniff",
    "X-Frame-Options": "DENY",
}

ConsoleDeviceId = Annotated[
    str,
    Path(min_length=1, max_length=64, pattern=DEVICE_ID_PATTERN),
]


class ConsoleModel(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)


class ConsoleDeviceSummary(ConsoleModel):
    device_id: str
    name: str | None
    online: bool
    online_threshold_ms: int
    last_seen_at_ms: int | None
    last_seen_age_ms: int | None
    last_telemetry_at_ms: int | None
    last_health_at_ms: int | None
    reported_health_level: (
        Literal["healthy", "degraded", "action_required", "unknown"] | None
    )
    firmware_version: str | None
    desired_config_revision: int
    latest_reported_config_revision: int | None
    highest_applied_config_revision: int
    config_sync: Literal["unknown", "in_sync", "pending", "regressed", "divergent"]
    latest_state: PresenceState | None


class ConsoleDeviceList(ConsoleModel):
    server_utc_ms: int
    items: list[ConsoleDeviceSummary]


class ConsoleWindow(ConsoleModel):
    start_ms: int
    end_ms: int
    hours: float
    time_basis: Literal["observed_at_ms_with_receive_fallback"]
    bucket_ms: int
    sample_count: int
    returned_points: int
    downsampled: bool
    transition_count: int
    transitions_truncated: bool
    feedback_count: int
    feedback_truncated: bool


class ConsoleFeedbackCounts(ConsoleModel):
    present: int
    absent: int
    mismatch: int


class ConsoleCalibration(ConsoleModel):
    pir_active_fraction: float | None
    sound_active_fraction: float | None
    present_fraction: float | None
    mic_envelope_mean: float | None
    mic_envelope_max: float | None
    noise_floor_mean: float | None
    sound_threshold_mean: float | None
    threshold_to_noise_ratio: float | None
    transition_reasons: dict[str, int]
    feedback: ConsoleFeedbackCounts


class ConsoleSeriesPoint(ConsoleModel):
    start_ms: int
    end_ms: int
    sample_count: int
    pir_active_fraction: float
    sound_active_fraction: float
    present_fraction: float
    mic_envelope_mean: float
    mic_envelope_max: float
    noise_floor_mean: float
    sound_threshold_mean: float
    brightness_mean: float


class ConsoleTransition(ConsoleModel):
    marker_ms: int
    boot_id: str
    seq: int
    uptime_ms: int
    build_id: str | None
    applied_config_revision: int | None
    from_state: PresenceState | None
    to_state: PresenceState
    reason: str
    pir: bool | None
    pir_age_ms: int | None
    sound_active: bool | None
    sound_age_ms: int | None
    mic_envelope: float | None
    noise_floor: float | None
    sound_threshold: float | None
    brightness_before: int | None
    brightness_after: int | None


class ConsoleFeedback(ConsoleModel):
    marker_ms: int
    feedback_id: str
    boot_id: str | None
    seq: int | None
    actual_presence: Literal["present", "absent"]
    observed_state: PresenceState | None
    source: str
    note: str | None


class ConsoleSnapshot(ConsoleModel):
    server_utc_ms: int
    device: ConsoleDeviceSummary
    window: ConsoleWindow
    latest: SampleOut | None
    calibration: ConsoleCalibration
    series: list[ConsoleSeriesPoint]
    transitions: list[ConsoleTransition]
    feedback: list[ConsoleFeedback]
    config: ConfigResponse


def _config_sync(
    device: Device, latest_reported_revision: int | None
) -> Literal["unknown", "in_sync", "pending", "regressed", "divergent"]:
    if latest_reported_revision is None:
        return "unknown"
    if latest_reported_revision < device.applied_config_revision:
        return "regressed"
    if latest_reported_revision == device.desired_config_revision:
        return "in_sync"
    if latest_reported_revision < device.desired_config_revision:
        return "pending"
    return "divergent"


def _device_summary(
    device: Device,
    latest_state: str | None,
    latest_telemetry_received_at_ms: int | None,
    latest_health_received_at_ms: int | None,
    latest_health_level: str | None,
    latest_reported_config_revision: int | None,
    now_ms: int,
) -> ConsoleDeviceSummary:
    activity_values = [
        value
        for value in (
            latest_telemetry_received_at_ms,
            latest_health_received_at_ms,
        )
        if value is not None
    ]
    latest_activity_at_ms = max(activity_values) if activity_values else None
    age_ms = (
        max(0, now_ms - latest_activity_at_ms)
        if latest_activity_at_ms is not None
        else None
    )
    return ConsoleDeviceSummary(
        device_id=device.device_id,
        name=device.name,
        online=age_ms is not None and age_ms <= ONLINE_THRESHOLD_MS,
        online_threshold_ms=ONLINE_THRESHOLD_MS,
        last_seen_at_ms=latest_activity_at_ms,
        last_seen_age_ms=age_ms,
        last_telemetry_at_ms=latest_telemetry_received_at_ms,
        last_health_at_ms=latest_health_received_at_ms,
        reported_health_level=latest_health_level,
        firmware_version=device.firmware_version,
        desired_config_revision=device.desired_config_revision,
        latest_reported_config_revision=latest_reported_config_revision,
        highest_applied_config_revision=device.applied_config_revision,
        config_sync=_config_sync(device, latest_reported_config_revision),
        latest_state=latest_state,
    )


def _optional_float(value: Any) -> float | None:
    return float(value) if value is not None else None


def _telemetry_event_window(start_ms: int, end_ms: int):
    """Match the expression used by idx_records_device_event."""
    event_ms = func.coalesce(
        TelemetryRecord.observed_at_ms, TelemetryRecord.received_at_ms
    )
    return and_(event_ms >= start_ms, event_ms <= end_ms)


def _asset_response(name: str, media_type: str) -> Response:
    body = (
        files("presence_api")
        .joinpath("console_assets")
        .joinpath(name)
        .read_text(encoding="utf-8")
    )
    return Response(
        body,
        media_type=media_type,
        headers={**_SECURITY_HEADERS, "Cache-Control": "no-cache"},
    )


def _console_request_origin_is_allowed(request: Request, console_host: str) -> bool:
    raw_headers = request.scope.get("headers", ())
    host_headers = [value for name, value in raw_headers if name.lower() == b"host"]
    if len(host_headers) != 1:
        return False
    try:
        host_value = host_headers[0].decode("ascii")
        parsed_host = urlsplit(f"//{host_value}")
        host_name = parsed_host.hostname
        host_port = parsed_host.port
    except (UnicodeDecodeError, ValueError):
        return False
    if (
        host_name is None
        or host_name.lower() != console_host.lower()
        or parsed_host.username is not None
        or parsed_host.password is not None
        or parsed_host.path
        or parsed_host.query
        or parsed_host.fragment
    ):
        return False

    origin_headers = [value for name, value in raw_headers if name.lower() == b"origin"]
    if not origin_headers:
        return request.method in {"GET", "HEAD", "OPTIONS"}
    if len(origin_headers) != 1:
        return False
    try:
        parsed_origin = urlsplit(origin_headers[0].decode("ascii"))
        origin_port = parsed_origin.port
    except (UnicodeDecodeError, ValueError):
        return False
    return (
        parsed_origin.scheme == "http"
        and parsed_origin.hostname is not None
        and parsed_origin.hostname.lower() == host_name.lower()
        and origin_port == host_port
        and parsed_origin.username is None
        and parsed_origin.password is None
        and parsed_origin.path in ("", "/")
        and not parsed_origin.query
        and not parsed_origin.fragment
    )


def _console_access_rules(
    console_host: str,
    console_tailnet_host: str | None,
    console_tailnet_client: str | None,
) -> tuple[tuple[IPv4Network, str], ...]:
    rules = [(_CONSOLE_LAN_NETWORK, console_host)]
    if console_tailnet_host is not None and console_tailnet_client is not None:
        try:
            client_address = ip_address(console_tailnet_client)
        except ValueError:
            return tuple(rules)
        if client_address.version == 4:
            rules.append((ip_network(f"{client_address}/32"), console_tailnet_host))
    return tuple(rules)


def require_console_access(
    request: Request,
    console_host: str = "192.168.0.46",
    console_tailnet_host: str | None = None,
    console_tailnet_client: str | None = None,
) -> None:
    """Keep the tokenless operator console on an exact trusted origin."""
    if request.client is None:
        raise HTTPException(
            status_code=403, detail="console is available only on trusted networks"
        )
    try:
        client_address = ip_address(request.client.host)
    except ValueError as error:
        raise HTTPException(
            status_code=403, detail="console is available only on trusted networks"
        ) from error
    allowed_hosts = tuple(
        allowed_host
        for network, allowed_host in _console_access_rules(
            console_host, console_tailnet_host, console_tailnet_client
        )
        if client_address in network
    )
    if not allowed_hosts:
        raise HTTPException(
            status_code=403, detail="console is available only on trusted networks"
        )
    # The peer address above remains the authorization boundary. This
    # additional Host/Origin gate prevents an allowed browser from being driven
    # through a DNS-rebinding origin into the tokenless Console. Pairing each
    # source range with its own host also rejects LAN/Tailnet origin crossover.
    if not any(
        _console_request_origin_is_allowed(request, allowed_host)
        for allowed_host in allowed_hosts
    ):
        raise HTTPException(status_code=403, detail="console origin is not allowed")


def create_console_router(
    database: Database,
    service: PresenceService,
    operations: OperationsService,
    console_host: str = "192.168.0.46",
    console_tailnet_host: str | None = None,
    console_tailnet_client: str | None = None,
) -> APIRouter:
    def require_configured_console_access(request: Request) -> None:
        require_console_access(
            request, console_host, console_tailnet_host, console_tailnet_client
        )

    router = APIRouter(
        dependencies=[Depends(require_configured_console_access)],
        responses={
            403: {"description": "Console is available only on trusted networks"}
        },
    )

    @router.get("/console", include_in_schema=False)
    def console_page() -> Response:
        return _asset_response("console.html", "text/html; charset=utf-8")

    @router.get("/console/assets/{asset_name}", include_in_schema=False)
    def console_asset(asset_name: str) -> Response:
        media_type = _ASSET_MEDIA_TYPES.get(asset_name)
        if media_type is None:
            raise HTTPException(status_code=404, detail="console asset not found")
        return _asset_response(asset_name, media_type)

    @router.get("/v1/console/devices", response_model=ConsoleDeviceList)
    def list_console_devices() -> ConsoleDeviceList:
        now_ms = utc_now_ms()
        event_time = func.coalesce(
            TelemetryRecord.observed_at_ms, TelemetryRecord.received_at_ms
        )
        latest_state = (
            select(Sample.state)
            .join(
                TelemetryRecord,
                (TelemetryRecord.device_id == Sample.device_id)
                & (TelemetryRecord.boot_id == Sample.boot_id)
                & (TelemetryRecord.seq == Sample.seq),
            )
            .where(Sample.device_id == Device.device_id)
            .order_by(
                event_time.desc(),
                TelemetryRecord.boot_id.desc(),
                TelemetryRecord.seq.desc(),
            )
            .limit(1)
            .correlate(Device)
            .scalar_subquery()
        )
        latest_reported_config_revision = (
            select(TelemetryRecord.applied_config_revision)
            .where(TelemetryRecord.device_id == Device.device_id)
            .order_by(
                TelemetryRecord.received_at_ms.desc(),
                TelemetryRecord.boot_id.desc(),
                TelemetryRecord.seq.desc(),
            )
            .limit(1)
            .correlate(Device)
            .scalar_subquery()
        )
        latest_telemetry_received_at_ms = (
            select(func.max(TelemetryRecord.received_at_ms))
            .where(TelemetryRecord.device_id == Device.device_id)
            .correlate(Device)
            .scalar_subquery()
        )
        latest_health_received_at_ms = (
            select(func.max(DeviceHealthReport.received_at_ms))
            .where(DeviceHealthReport.device_id == Device.device_id)
            .correlate(Device)
            .scalar_subquery()
        )
        latest_health_level = (
            select(DeviceHealthReport.local_level)
            .where(DeviceHealthReport.device_id == Device.device_id)
            .order_by(
                DeviceHealthReport.received_at_ms.desc(),
                DeviceHealthReport.boot_id.desc(),
                DeviceHealthReport.sequence.desc(),
            )
            .limit(1)
            .correlate(Device)
            .scalar_subquery()
        )
        with database.session() as session:
            rows = session.execute(
                select(
                    Device,
                    latest_state.label("latest_state"),
                    latest_telemetry_received_at_ms.label(
                        "latest_telemetry_received_at_ms"
                    ),
                    latest_health_received_at_ms.label("latest_health_received_at_ms"),
                    latest_health_level.label("latest_health_level"),
                    latest_reported_config_revision.label(
                        "latest_reported_config_revision"
                    ),
                ).order_by(
                    latest_health_received_at_ms.desc(),
                    latest_telemetry_received_at_ms.desc(),
                    Device.device_id,
                )
            ).all()
            items = [
                _device_summary(
                    device,
                    state,
                    last_telemetry_ms,
                    last_health_ms,
                    health_level,
                    latest_revision,
                    now_ms,
                )
                for (
                    device,
                    state,
                    last_telemetry_ms,
                    last_health_ms,
                    health_level,
                    latest_revision,
                ) in rows
            ]
        return ConsoleDeviceList(server_utc_ms=now_ms, items=items)

    @router.get(
        "/v1/console/devices/{device_id}/snapshot",
        response_model=ConsoleSnapshot,
        responses={404: {"description": "Device does not exist"}},
    )
    def console_snapshot(
        device_id: ConsoleDeviceId,
        hours: Annotated[float, Query(ge=0.25, le=24.0)] = 6.0,
        max_points: Annotated[int, Query(ge=60, le=2_000)] = 480,
    ) -> ConsoleSnapshot:
        now_ms = utc_now_ms()
        window_ms = max(1, round(hours * 3_600_000))
        start_ms = max(0, now_ms - window_ms)
        # The query window includes both endpoints, so it contains
        # ``window_ms + 1`` possible integer-millisecond timestamps. Size
        # buckets from that inclusive width to keep the response at or below
        # ``max_points`` even when samples land exactly on both endpoints.
        bucket_ms = max(1_000, ceil((window_ms + 1) / max_points))
        received_ms = TelemetryRecord.received_at_ms
        event_ms = func.coalesce(TelemetryRecord.observed_at_ms, received_ms)
        event_window = _telemetry_event_window(start_ms, now_ms)
        bucket = cast((event_ms - start_ms) / bucket_ms, Integer)
        pir_fraction = func.avg(cast(Sample.pir, Integer))
        sound_fraction = func.avg(cast(Sample.sound_active, Integer))
        present_fraction = func.avg(
            case((Sample.state == PresenceState.present.value, 1.0), else_=0.0)
        )

        sample_join = (
            select(TelemetryRecord, Sample)
            .join(
                Sample,
                (Sample.device_id == TelemetryRecord.device_id)
                & (Sample.boot_id == TelemetryRecord.boot_id)
                & (Sample.seq == TelemetryRecord.seq),
            )
            .where(
                TelemetryRecord.device_id == device_id,
                event_window,
            )
        )

        with database.session() as session:
            device = session.get(Device, device_id)
            if device is None:
                raise HTTPException(status_code=404, detail="device not found")

            latest_state = session.execute(
                select(Sample.state)
                .join(
                    TelemetryRecord,
                    (TelemetryRecord.device_id == Sample.device_id)
                    & (TelemetryRecord.boot_id == Sample.boot_id)
                    & (TelemetryRecord.seq == Sample.seq),
                )
                .where(Sample.device_id == device_id)
                .order_by(
                    event_ms.desc(),
                    TelemetryRecord.boot_id.desc(),
                    TelemetryRecord.seq.desc(),
                )
                .limit(1)
            ).scalar_one_or_none()
            latest_telemetry_received_at_ms = session.execute(
                select(func.max(TelemetryRecord.received_at_ms)).where(
                    TelemetryRecord.device_id == device_id
                )
            ).scalar_one()
            latest_health = session.scalars(
                select(DeviceHealthReport)
                .where(DeviceHealthReport.device_id == device_id)
                .order_by(
                    DeviceHealthReport.received_at_ms.desc(),
                    DeviceHealthReport.boot_id.desc(),
                    DeviceHealthReport.sequence.desc(),
                )
                .limit(1)
            ).first()
            latest_reported_config_revision = session.execute(
                select(TelemetryRecord.applied_config_revision)
                .where(TelemetryRecord.device_id == device_id)
                .order_by(
                    TelemetryRecord.received_at_ms.desc(),
                    TelemetryRecord.boot_id.desc(),
                    TelemetryRecord.seq.desc(),
                )
                .limit(1)
            ).scalar_one_or_none()

            aggregate = session.execute(
                sample_join.with_only_columns(
                    func.count().label("sample_count"),
                    pir_fraction.label("pir_active_fraction"),
                    sound_fraction.label("sound_active_fraction"),
                    present_fraction.label("present_fraction"),
                    func.avg(Sample.mic_envelope).label("mic_envelope_mean"),
                    func.max(Sample.mic_envelope).label("mic_envelope_max"),
                    func.avg(Sample.noise_floor).label("noise_floor_mean"),
                    func.avg(Sample.sound_threshold).label("threshold_mean"),
                )
            ).one()

            series_rows = session.execute(
                sample_join.with_only_columns(
                    bucket.label("bucket"),
                    func.min(event_ms).label("start_ms"),
                    func.max(event_ms).label("end_ms"),
                    func.count().label("sample_count"),
                    pir_fraction.label("pir_active_fraction"),
                    sound_fraction.label("sound_active_fraction"),
                    present_fraction.label("present_fraction"),
                    func.avg(Sample.mic_envelope).label("mic_envelope_mean"),
                    func.max(Sample.mic_envelope).label("mic_envelope_max"),
                    func.avg(Sample.noise_floor).label("noise_floor_mean"),
                    func.avg(Sample.sound_threshold).label("threshold_mean"),
                    func.avg(Sample.brightness).label("brightness_mean"),
                )
                .group_by(bucket)
                .order_by(bucket)
            ).all()

            transition_event_ms = event_ms
            transition_rows = session.execute(
                select(TelemetryRecord, StateTransition, transition_event_ms)
                .join(
                    StateTransition,
                    (StateTransition.device_id == TelemetryRecord.device_id)
                    & (StateTransition.boot_id == TelemetryRecord.boot_id)
                    & (StateTransition.seq == TelemetryRecord.seq),
                )
                .where(
                    TelemetryRecord.device_id == device_id,
                    event_window,
                )
                .order_by(
                    transition_event_ms,
                    TelemetryRecord.boot_id,
                    TelemetryRecord.seq,
                )
                .limit(MAX_EVENT_MARKERS + 1)
            ).all()

            transition_reason_rows = session.execute(
                select(StateTransition.reason, func.count())
                .join(
                    TelemetryRecord,
                    (TelemetryRecord.device_id == StateTransition.device_id)
                    & (TelemetryRecord.boot_id == StateTransition.boot_id)
                    & (TelemetryRecord.seq == StateTransition.seq),
                )
                .where(
                    TelemetryRecord.device_id == device_id,
                    event_window,
                )
                .group_by(StateTransition.reason)
            ).all()

            feedback_event_ms = func.coalesce(
                Feedback.occurred_at_ms,
                TelemetryRecord.received_at_ms,
                Feedback.created_at_ms,
            )
            feedback_record_join = and_(
                TelemetryRecord.device_id == Feedback.device_id,
                TelemetryRecord.boot_id == Feedback.boot_id,
                TelemetryRecord.seq == Feedback.seq,
            )
            feedback_rows = session.execute(
                select(Feedback, feedback_event_ms)
                .outerjoin(TelemetryRecord, feedback_record_join)
                .where(
                    Feedback.device_id == device_id,
                    feedback_event_ms >= start_ms,
                    feedback_event_ms <= now_ms,
                )
                .order_by(feedback_event_ms, Feedback.feedback_id)
                .limit(MAX_EVENT_MARKERS + 1)
            ).all()
            feedback_aggregate = session.execute(
                select(
                    func.count().label("feedback_count"),
                    func.sum(
                        case((Feedback.actual_presence == "present", 1), else_=0)
                    ).label("present_count"),
                    func.sum(
                        case((Feedback.actual_presence == "absent", 1), else_=0)
                    ).label("absent_count"),
                    func.sum(
                        case(
                            (
                                and_(
                                    Feedback.observed_state.is_not(None),
                                    or_(
                                        and_(
                                            Feedback.actual_presence == "present",
                                            Feedback.observed_state
                                            != PresenceState.present.value,
                                        ),
                                        and_(
                                            Feedback.actual_presence == "absent",
                                            Feedback.observed_state
                                            == PresenceState.present.value,
                                        ),
                                    ),
                                ),
                                1,
                            ),
                            else_=0,
                        )
                    ).label("mismatch_count"),
                )
                .outerjoin(TelemetryRecord, feedback_record_join)
                .where(
                    Feedback.device_id == device_id,
                    feedback_event_ms >= start_ms,
                    feedback_event_ms <= now_ms,
                )
            ).one()

        try:
            latest = service.latest_sample(device_id)
        except NotFoundError:
            latest = None
        config = service.get_config(device_id)

        transition_truncated = len(transition_rows) > MAX_EVENT_MARKERS
        feedback_truncated = len(feedback_rows) > MAX_EVENT_MARKERS
        visible_transitions = transition_rows[:MAX_EVENT_MARKERS]
        visible_feedback = feedback_rows[:MAX_EVENT_MARKERS]
        feedback_count = int(feedback_aggregate.feedback_count or 0)
        feedback_present = int(feedback_aggregate.present_count or 0)
        feedback_absent = int(feedback_aggregate.absent_count or 0)
        feedback_mismatch = int(feedback_aggregate.mismatch_count or 0)
        noise_mean = _optional_float(aggregate.noise_floor_mean)
        threshold_mean = _optional_float(aggregate.threshold_mean)
        threshold_ratio = (
            threshold_mean / noise_mean
            if threshold_mean is not None and noise_mean not in (None, 0.0)
            else None
        )

        series = [
            ConsoleSeriesPoint(
                start_ms=row.start_ms,
                end_ms=row.end_ms,
                sample_count=row.sample_count,
                pir_active_fraction=float(row.pir_active_fraction),
                sound_active_fraction=float(row.sound_active_fraction),
                present_fraction=float(row.present_fraction),
                mic_envelope_mean=float(row.mic_envelope_mean),
                mic_envelope_max=float(row.mic_envelope_max),
                noise_floor_mean=float(row.noise_floor_mean),
                sound_threshold_mean=float(row.threshold_mean),
                brightness_mean=float(row.brightness_mean),
            )
            for row in series_rows
        ]
        transitions = [
            ConsoleTransition(
                marker_ms=event_ms,
                boot_id=record.boot_id,
                seq=record.seq,
                uptime_ms=record.uptime_ms,
                build_id=record.build_id,
                applied_config_revision=record.applied_config_revision,
                from_state=transition.from_state,
                to_state=transition.to_state,
                reason=transition.reason,
                pir=transition.pir,
                pir_age_ms=transition.pir_age_ms,
                sound_active=transition.sound_active,
                sound_age_ms=transition.sound_age_ms,
                mic_envelope=transition.mic_envelope,
                noise_floor=transition.noise_floor,
                sound_threshold=transition.sound_threshold,
                brightness_before=transition.brightness_before,
                brightness_after=transition.brightness_after,
            )
            for record, transition, event_ms in visible_transitions
        ]
        feedback = [
            ConsoleFeedback(
                marker_ms=event_ms,
                feedback_id=row.feedback_id,
                boot_id=row.boot_id,
                seq=row.seq,
                actual_presence=row.actual_presence,
                observed_state=row.observed_state,
                source=row.source,
                note=row.note,
            )
            for row, event_ms in visible_feedback
        ]
        sample_count = int(aggregate.sample_count)
        return ConsoleSnapshot(
            server_utc_ms=now_ms,
            device=_device_summary(
                device,
                latest_state,
                latest_telemetry_received_at_ms,
                latest_health.received_at_ms if latest_health is not None else None,
                latest_health.local_level if latest_health is not None else None,
                latest_reported_config_revision,
                now_ms,
            ),
            window=ConsoleWindow(
                start_ms=start_ms,
                end_ms=now_ms,
                hours=hours,
                time_basis="observed_at_ms_with_receive_fallback",
                bucket_ms=bucket_ms,
                sample_count=sample_count,
                returned_points=len(series),
                downsampled=sample_count > len(series),
                transition_count=sum(
                    count for _reason, count in transition_reason_rows
                ),
                transitions_truncated=transition_truncated,
                feedback_count=feedback_count,
                feedback_truncated=feedback_truncated,
            ),
            latest=latest,
            calibration=ConsoleCalibration(
                pir_active_fraction=_optional_float(aggregate.pir_active_fraction),
                sound_active_fraction=_optional_float(aggregate.sound_active_fraction),
                present_fraction=_optional_float(aggregate.present_fraction),
                mic_envelope_mean=_optional_float(aggregate.mic_envelope_mean),
                mic_envelope_max=_optional_float(aggregate.mic_envelope_max),
                noise_floor_mean=noise_mean,
                sound_threshold_mean=threshold_mean,
                threshold_to_noise_ratio=threshold_ratio,
                transition_reasons={
                    reason: count for reason, count in transition_reason_rows
                },
                feedback=ConsoleFeedbackCounts(
                    present=feedback_present,
                    absent=feedback_absent,
                    mismatch=feedback_mismatch,
                ),
            ),
            series=series,
            transitions=transitions,
            feedback=feedback,
            config=config,
        )

    @router.put(
        "/v1/console/devices/{device_id}/config",
        response_model=ConfigResponse,
        responses={409: {"description": "Stale configuration revision"}},
    )
    def put_console_config(
        device_id: ConsoleDeviceId, update: ConfigPut
    ) -> ConfigResponse:
        return service.put_config(device_id, update)

    @router.get(
        "/v1/console/devices/{device_id}/health",
        response_model=DeviceHealthPage,
    )
    def console_device_health(
        device_id: ConsoleDeviceId,
        limit: Annotated[int, Query(ge=1, le=1_440)] = 120,
    ) -> DeviceHealthPage:
        return operations.health_page(device_id, limit)

    @router.get(
        "/v1/console/devices/{device_id}/commands",
        response_model=list[CommandOut],
    )
    def console_commands(
        device_id: ConsoleDeviceId,
        limit: Annotated[int, Query(ge=1, le=256)] = 50,
    ) -> list[CommandOut]:
        return operations.list_commands(device_id, limit)

    @router.post(
        "/v1/console/devices/{device_id}/commands",
        response_model=CommandOut,
    )
    def create_console_command(
        device_id: ConsoleDeviceId, request: ConsoleCommandCreate
    ) -> CommandOut:
        return operations.create_command(device_id, request)

    @router.get(
        "/v1/console/devices/{device_id}/logs",
        response_model=OperationalLogPage,
    )
    def console_operational_logs(
        device_id: ConsoleDeviceId,
        limit: Annotated[int, Query(ge=1, le=1_000)] = 200,
        since_ms: Annotated[int | None, Query(ge=0)] = None,
    ) -> OperationalLogPage:
        return operations.list_operational_logs(device_id, limit, since_ms)

    @router.get(
        "/v1/console/devices/{device_id}/coredumps",
        response_model=SanitizedCoreDumpPage,
        description=(
            "Sanitized symbolication summaries only. Raw core-dump bytes are "
            "never exposed by a tokenless Console route."
        ),
    )
    def console_coredumps(
        device_id: ConsoleDeviceId,
        limit: Annotated[int, Query(ge=1, le=10)] = 10,
    ) -> SanitizedCoreDumpPage:
        return operations.list_coredump_summaries(device_id, limit)

    @router.get("/v1/console/releases", response_model=list[ReleaseSummary])
    def console_releases() -> list[ReleaseSummary]:
        return operations.list_releases()

    @router.post(
        "/v1/console/releases/import",
        response_model=ReleaseSummary,
        responses={
            400: {"description": "Bundle or signature verification failed"},
            409: {"description": "Release identity conflict"},
        },
    )
    def import_console_release(request: ReleaseBundleImport) -> ReleaseSummary:
        return operations.import_release(request)

    @router.get(
        "/v1/console/devices/{device_id}/release",
        response_model=DeviceReleaseTargetOut | None,
    )
    def console_release_target(
        device_id: ConsoleDeviceId,
    ) -> DeviceReleaseTargetOut | None:
        return operations.get_release_target(device_id)

    @router.get(
        "/v1/console/devices/{device_id}/release-status",
        response_model=DeviceReleaseOverview,
    )
    def console_release_status(device_id: ConsoleDeviceId) -> DeviceReleaseOverview:
        return operations.release_overview(device_id)

    @router.put(
        "/v1/console/devices/{device_id}/release",
        response_model=DeviceReleaseTargetOut,
        responses={
            404: {"description": "Verified release was not found"},
            409: {"description": "Wireless downgrade was rejected"},
        },
    )
    def select_console_release(
        device_id: ConsoleDeviceId, selection: DeviceReleaseSelection
    ) -> DeviceReleaseTargetOut:
        return operations.select_release(device_id, selection)

    return router
