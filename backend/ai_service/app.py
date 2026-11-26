from __future__ import annotations

import asyncio
import hmac
import os
from contextlib import asynccontextmanager
from typing import Annotated

from fastapi import Depends, FastAPI, HTTPException, status
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from prometheus_client import CONTENT_TYPE_LATEST, REGISTRY, Gauge, generate_latest
from pydantic import BaseModel, ConfigDict, Field
from starlette.responses import Response

from features import FeatureEngineer, TelemetrySample, metric_node
from model import AnomalyModel


ROOM_LABEL = "101"
ANOMALY_THRESHOLD = 0.65
CRITICAL_SPIKE_THRESHOLD = 0.80
API_TOKEN = os.environ.get("API_TOKEN", "")

anomaly_score_metric = Gauge(
    "airpulse_anomaly_score",
    "Normalized AirPulse anomaly score from zero (normal) to one (severe).",
    ["room", "node"],
    registry=REGISTRY,
)
anomaly_flag_metric = Gauge(
    "airpulse_anomaly_flag",
    "Whether an AirPulse anomaly has been confirmed or a sensor fault detected.",
    ["room", "node"],
    registry=REGISTRY,
)
co2_metric = Gauge(
    "airpulse_co2_ppm",
    "Carbon dioxide concentration in ppm.",
    ["room", "node"],
    registry=REGISTRY,
)
pm25_metric = Gauge(
    "airpulse_pm25_ugm3",
    "PM2.5 concentration in micrograms per cubic meter.",
    ["room", "node"],
    registry=REGISTRY,
)
pm10_metric = Gauge(
    "airpulse_pm10_ugm3",
    "PM10 concentration in micrograms per cubic meter.",
    ["room", "node"],
    registry=REGISTRY,
)
temperature_metric = Gauge(
    "airpulse_temperature_celsius",
    "Air temperature in degrees Celsius.",
    ["room", "node"],
    registry=REGISTRY,
)
humidity_metric = Gauge(
    "airpulse_humidity_percent",
    "Relative humidity in percent.",
    ["room", "node"],
    registry=REGISTRY,
)
wifi_metric = Gauge(
    "airpulse_wifi_rssi_dbm",
    "Wi-Fi received signal strength in dBm.",
    ["room", "node"],
    registry=REGISTRY,
)

bearer_scheme = HTTPBearer(auto_error=False)


class TelemetryRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    wifi: int
    rco2: float
    pm02: float
    pm10: float
    atmp: float
    rhum: float
    serialno: str = Field(min_length=1, max_length=128)


class TelemetryResponse(BaseModel):
    status: str
    anomaly_score: float
    anomaly_flag: int
    reason: str


@asynccontextmanager
async def lifespan(application: FastAPI):
    if not API_TOKEN:
        raise RuntimeError("API_TOKEN must be configured")
    application.state.model = await asyncio.to_thread(AnomalyModel.load_or_train)
    application.state.features = FeatureEngineer()
    application.state.telemetry_lock = asyncio.Lock()
    yield


app = FastAPI(title="AirPulse AI Telemetry", version="1.0.0", lifespan=lifespan)


def authenticate(
    credentials: Annotated[
        HTTPAuthorizationCredentials | None, Depends(bearer_scheme)
    ],
) -> None:
    if (
        credentials is None
        or credentials.scheme.lower() != "bearer"
        or not hmac.compare_digest(credentials.credentials, API_TOKEN)
    ):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid or missing bearer token",
            headers={"WWW-Authenticate": "Bearer"},
        )


@app.get("/health")
def health() -> dict[str, str]:
    if not hasattr(app.state, "model"):
        raise HTTPException(status_code=503, detail="Model is not ready")
    return {"status": "ok"}


@app.get("/metrics")
def metrics() -> Response:
    return Response(generate_latest(REGISTRY), media_type=CONTENT_TYPE_LATEST)


@app.post("/telemetry", response_model=TelemetryResponse)
async def ingest_telemetry(
    telemetry: TelemetryRequest,
    _: Annotated[None, Depends(authenticate)],
) -> TelemetryResponse:
    if not hasattr(app.state, "model"):
        raise HTTPException(status_code=503, detail="Model is not ready")

    sample = TelemetrySample(**telemetry.model_dump())
    node = metric_node(sample.serialno)
    async with app.state.telemetry_lock:
        feature_result = app.state.features.process(sample)
        if feature_result.fault_reason is not None:
            score = 1.0
            reason = feature_result.fault_reason
        else:
            raw_score = app.state.model.score(feature_result.values)
            score = app.state.features.smooth_anomaly_score(
                sample.serialno, raw_score
            )
            if score > CRITICAL_SPIKE_THRESHOLD:
                reason = "critical_spike_candidate"
            elif score > ANOMALY_THRESHOLD:
                reason = "model_anomaly_candidate"
            else:
                reason = "normal"

        flag = app.state.features.update_anomaly_streak(
            sample.serialno, score, feature_result.fault_reason, ANOMALY_THRESHOLD
        )
        if feature_result.fault_reason is None and flag == 1:
            reason = (
                "critical_spike_confirmed"
                if score > CRITICAL_SPIKE_THRESHOLD
                else "model_anomaly_confirmed"
            )

        labels = {"room": ROOM_LABEL, "node": node}
        anomaly_score_metric.labels(**labels).set(score)
        anomaly_flag_metric.labels(**labels).set(flag)
        co2_metric.labels(**labels).set(sample.rco2)
        pm25_metric.labels(**labels).set(sample.pm02)
        pm10_metric.labels(**labels).set(sample.pm10)
        temperature_metric.labels(**labels).set(sample.atmp)
        humidity_metric.labels(**labels).set(sample.rhum)
        wifi_metric.labels(**labels).set(sample.wifi)

    return TelemetryResponse(
        status="ok",
        anomaly_score=round(score, 2),
        anomaly_flag=flag,
        reason=reason,
    )
