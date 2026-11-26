from __future__ import annotations

import math
import time
from collections import deque
from dataclasses import dataclass, field


FEATURE_NAMES = (
    "rco2",
    "d_co2_dt",
    "pm02",
    "pm10",
    "pm_ratio",
    "delta_temp",
    "delta_hum",
)

EMA_WINDOW_SECONDS = 15 * 60
HISTORY_RETENTION_SECONDS = 15 * 60
ANOMALY_SCORE_EMA_ALPHA = 0.5


@dataclass(frozen=True)
class TelemetrySample:
    wifi: int
    rco2: float
    pm02: float
    pm10: float
    atmp: float
    rhum: float
    serialno: str

    def sensor_signature(self) -> tuple[float, ...]:
        return (self.rco2, self.pm02, self.pm10, self.atmp, self.rhum)


@dataclass
class DeviceState:
    history: deque[tuple[float, float, float, float]] = field(
        default_factory=lambda: deque(maxlen=400)
    )
    last_sample: TelemetrySample | None = None
    last_seen_at: float | None = None
    ema_temperature: float | None = None
    ema_humidity: float | None = None
    ema_anomaly_score: float | None = None
    identical_samples: int = 0
    consecutive_high_scores: int = 0


@dataclass(frozen=True)
class FeatureResult:
    values: dict[str, float]
    fault_reason: str | None


class FeatureEngineer:
    """Build per-node rate, particulate, and EMA-delta features."""

    def __init__(self) -> None:
        self.devices: dict[str, DeviceState] = {}

    def process(
        self, sample: TelemetrySample, now: float | None = None
    ) -> FeatureResult:
        now = time.monotonic() if now is None else now
        state = self.devices.setdefault(sample.serialno, DeviceState())
        fault_reason = self._fault_reason(sample, state)

        if fault_reason is not None:
            return FeatureResult(
                values={name: 0.0 for name in FEATURE_NAMES},
                fault_reason=fault_reason,
            )

        previous = state.last_sample
        elapsed = 3.0 if state.last_seen_at is None else max(now - state.last_seen_at, 0.001)
        d_co2_dt = (
            0.0 if previous is None else (sample.rco2 - previous.rco2) / elapsed
        )

        if state.ema_temperature is None or state.ema_humidity is None:
            delta_temp = 0.0
            delta_hum = 0.0
            state.ema_temperature = sample.atmp
            state.ema_humidity = sample.rhum
        else:
            delta_temp = sample.atmp - state.ema_temperature
            delta_hum = sample.rhum - state.ema_humidity
            alpha = 2.0 * elapsed / (EMA_WINDOW_SECONDS + elapsed)
            state.ema_temperature += alpha * delta_temp
            state.ema_humidity += alpha * delta_hum

        state.history.append((now, sample.rco2, sample.atmp, sample.rhum))
        while state.history and now - state.history[0][0] > HISTORY_RETENTION_SECONDS:
            state.history.popleft()

        state.last_sample = sample
        state.last_seen_at = now

        return FeatureResult(
            values=self._build_values(sample, d_co2_dt, delta_temp, delta_hum),
            fault_reason=None,
        )

    def update_anomaly_streak(
        self,
        serialno: str,
        score: float,
        fault_reason: str | None,
        threshold: float = 0.65,
    ) -> int:
        state = self.devices.setdefault(serialno, DeviceState())
        if fault_reason is not None:
            state.consecutive_high_scores = 0
            return 1

        if score > threshold:
            state.consecutive_high_scores += 1
        else:
            state.consecutive_high_scores = 0
        return int(state.consecutive_high_scores >= 3)

    def smooth_anomaly_score(self, serialno: str, score: float) -> float:
        state = self.devices.setdefault(serialno, DeviceState())
        if state.ema_anomaly_score is None:
            state.ema_anomaly_score = score
        else:
            state.ema_anomaly_score = (
                ANOMALY_SCORE_EMA_ALPHA * score
                + (1.0 - ANOMALY_SCORE_EMA_ALPHA) * state.ema_anomaly_score
            )
        return state.ema_anomaly_score

    @staticmethod
    def _build_values(
        sample: TelemetrySample,
        d_co2_dt: float,
        delta_temp: float,
        delta_hum: float,
    ) -> dict[str, float]:
        return {
            "rco2": sample.rco2,
            "d_co2_dt": d_co2_dt,
            "pm02": sample.pm02,
            "pm10": sample.pm10,
            "pm_ratio": sample.pm02 / max(sample.pm10, 1.0),
            "delta_temp": delta_temp,
            "delta_hum": delta_hum,
        }

    @staticmethod
    def _fault_reason(sample: TelemetrySample, state: DeviceState) -> str | None:
        measured = {
            "rco2": sample.rco2,
            "pm02": sample.pm02,
            "pm10": sample.pm10,
            "atmp": sample.atmp,
            "rhum": sample.rhum,
        }
        non_finite = [name for name, value in measured.items() if not math.isfinite(value)]
        if non_finite:
            return "sensor_fault:non_finite:" + ",".join(non_finite)

        out_of_bounds = []
        if not 250 <= sample.rco2 <= 10000:
            out_of_bounds.append("rco2")
        if not 0 <= sample.pm02 <= 1000:
            out_of_bounds.append("pm02")
        if not 0 <= sample.pm10 <= 1000:
            out_of_bounds.append("pm10")
        if not -40 <= sample.atmp <= 85:
            out_of_bounds.append("atmp")
        if not 0 <= sample.rhum <= 100:
            out_of_bounds.append("rhum")
        if not -127 <= sample.wifi <= 0:
            out_of_bounds.append("wifi")
        if sample.pm02 > sample.pm10:
            out_of_bounds.append("pm02_exceeds_pm10")
        if out_of_bounds:
            return "sensor_fault:out_of_bounds:" + ",".join(out_of_bounds)

        previous = state.last_sample
        if previous is not None and sample.sensor_signature() == previous.sensor_signature():
            state.identical_samples += 1
        else:
            state.identical_samples = 1
        if state.identical_samples > 10:
            return "sensor_fault:frozen_values"
        return None


def metric_node(serialno: str) -> str:
    """Turn a device serial into a bounded, readable Prometheus node label."""
    node = serialno.rsplit(":", 1)[-1]
    safe = "".join(
        char if char.isalnum() or char in "_.-" else "_" for char in node
    )
    return safe[:64] or "unknown"
