from __future__ import annotations

import hmac
from contextlib import asynccontextmanager
from typing import Annotated

from fastapi import (
    APIRouter,
    Depends,
    FastAPI,
    HTTPException,
    Query,
)
from fastapi import (
    Path as ApiPath,
)
from fastapi.responses import JSONResponse
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from sqlalchemy import text

from .config import Settings
from .console import create_console_router
from .database import Database
from .schemas import (
    DEVICE_ID_PATTERN,
    MAX_SIGNED_64,
    ConfigPut,
    ConfigResponse,
    FeedbackCreate,
    FeedbackPage,
    FeedbackResponse,
    HealthResponse,
    IngestResponse,
    SampleOut,
    SamplePage,
    TelemetryBatch,
    TransitionPage,
)
from .service import (
    ConflictError,
    NotFoundError,
    PresenceService,
    QueryWindow,
    decode_feedback_cursor,
    decode_telemetry_cursor,
    utc_now_ms,
)

DeviceId = Annotated[
    str,
    ApiPath(min_length=1, max_length=64, pattern=DEVICE_ID_PATTERN),
]
BEARER_AUTH = HTTPBearer(auto_error=False, scheme_name="PresenceBearer")


def create_app(settings: Settings | None = None) -> FastAPI:
    active_settings = settings or Settings.from_environment()
    database = Database(active_settings.database_path)
    service = PresenceService(database)

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        database.initialize()
        yield
        database.dispose()

    app = FastAPI(
        title="M5 Presence API",
        version="0.1.0",
        lifespan=lifespan,
    )

    @app.exception_handler(ConflictError)
    async def conflict_handler(_request, exc: ConflictError):
        return JSONResponse(status_code=409, content={"detail": str(exc)})

    @app.exception_handler(NotFoundError)
    async def not_found_handler(_request, exc: NotFoundError):
        return JSONResponse(status_code=404, content={"detail": str(exc)})

    def require_api_token(
        credentials: Annotated[
            HTTPAuthorizationCredentials | None,
            Depends(BEARER_AUTH),
        ],
    ) -> None:
        expected = active_settings.api_token
        if expected is None:
            return
        if (
            credentials is None
            or credentials.scheme.lower() != "bearer"
            or not hmac.compare_digest(credentials.credentials, expected)
        ):
            raise HTTPException(status_code=401, detail="invalid API token")

    @app.get("/v1/healthz", response_model=HealthResponse)
    def healthz() -> HealthResponse:
        with database.session() as session:
            session.execute(text("SELECT 1"))
        return HealthResponse(
            ok=True,
            database="ok",
            server_utc_ms=utc_now_ms(),
        )

    router = APIRouter(
        prefix="/v1/devices/{device_id}",
        dependencies=[Depends(require_api_token)],
        responses={401: {"description": "Missing or invalid bearer token"}},
    )

    @router.post(
        "/batches",
        response_model=IngestResponse,
        responses={409: {"description": "Idempotency or clock-anchor conflict"}},
    )
    def ingest_batch(device_id: DeviceId, batch: TelemetryBatch) -> IngestResponse:
        return service.ingest(device_id, batch)

    @router.get(
        "/latest",
        response_model=SampleOut,
        responses={404: {"description": "No sample exists for this device"}},
    )
    def latest_sample(device_id: DeviceId) -> SampleOut:
        return service.latest_sample(device_id)

    def query_window(
        start_ms: Annotated[
            int | None,
            Query(
                ge=0,
                le=MAX_SIGNED_64,
                description="Inclusive lower bound on server receive time",
            ),
        ] = None,
        end_ms: Annotated[
            int | None,
            Query(
                ge=0,
                le=MAX_SIGNED_64,
                description="Inclusive upper bound on server receive time",
            ),
        ] = None,
        limit: Annotated[int, Query(ge=1, le=5000)] = 500,
        cursor: Annotated[str | None, Query(max_length=512)] = None,
    ) -> QueryWindow:
        if start_ms is not None and end_ms is not None and end_ms < start_ms:
            raise HTTPException(status_code=400, detail="end_ms must be >= start_ms")
        try:
            decoded_cursor = (
                decode_telemetry_cursor(cursor) if cursor is not None else None
            )
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc
        return QueryWindow(
            start_ms=start_ms,
            end_ms=end_ms,
            limit=limit,
            cursor=decoded_cursor,
        )

    window_dependency = Depends(query_window)

    @router.get(
        "/samples",
        response_model=SamplePage,
        responses={400: {"description": "Invalid time window"}},
    )
    def samples(
        device_id: DeviceId,
        window: QueryWindow = window_dependency,
    ) -> SamplePage:
        return service.list_samples(device_id, window)

    @router.get(
        "/transitions",
        response_model=TransitionPage,
        responses={400: {"description": "Invalid time window"}},
    )
    def transitions(
        device_id: DeviceId,
        window: QueryWindow = window_dependency,
    ) -> TransitionPage:
        return service.list_transitions(device_id, window)

    @router.post(
        "/feedback",
        response_model=FeedbackResponse,
        responses={
            404: {"description": "Referenced telemetry record does not exist"},
            409: {"description": "Feedback idempotency conflict"},
        },
    )
    def create_feedback(
        device_id: DeviceId, feedback: FeedbackCreate
    ) -> FeedbackResponse:
        return service.create_feedback(device_id, feedback)

    @router.get(
        "/feedback",
        response_model=FeedbackPage,
        responses={400: {"description": "Invalid pagination cursor"}},
    )
    def feedback(
        device_id: DeviceId,
        limit: Annotated[int, Query(ge=1, le=5000)] = 500,
        cursor: Annotated[str | None, Query(max_length=512)] = None,
    ) -> FeedbackPage:
        try:
            decoded_cursor = (
                decode_feedback_cursor(cursor) if cursor is not None else None
            )
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc
        return service.list_feedback(device_id, limit, decoded_cursor)

    @router.get("/config", response_model=ConfigResponse)
    def get_config(device_id: DeviceId) -> ConfigResponse:
        return service.get_config(device_id)

    @router.put(
        "/config",
        response_model=ConfigResponse,
        responses={409: {"description": "Stale configuration revision"}},
    )
    def put_config(device_id: DeviceId, update: ConfigPut) -> ConfigResponse:
        return service.put_config(device_id, update)

    app.include_router(router)
    app.include_router(create_console_router(database, service))
    return app


app = create_app()
