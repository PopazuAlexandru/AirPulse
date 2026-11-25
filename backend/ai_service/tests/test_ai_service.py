from __future__ import annotations

from fastapi.testclient import TestClient
import pytest

import app as service
from features import FEATURE_NAMES
from features import FeatureEngineer, TelemetrySample
from model import AnomalyModel


def sample(**overrides: object) -> TelemetrySample:
    values: dict[str, object] = {
        "wifi": -52,
        "rco2": 850.0,
        "pm02": 8.0,
        "pm10": 12.0,
        "atmp": 22.4,
        "rhum": 45.1,
        "serialno": "esp32:30pin_01",
    }
    values.update(overrides)
    return TelemetrySample(**values)  # type: ignore[arg-type]


def test_feature_engineering_tracks_rate_ratio_and_ema_deltas() -> None:
    engineer = FeatureEngineer()
    first = engineer.process(sample(), now=100.0)
    second = engineer.process(
        sample(rco2=880.0, atmp=23.0, rhum=47.0), now=103.0
    )

    assert first.fault_reason is None
    assert second.values["d_co2_dt"] == 10.0
    assert second.values["pm_ratio"] == 8.0 / 12.0
    assert second.values["delta_temp"] > 0
    assert second.values["delta_hum"] > 0
    assert len(engineer.devices["esp32:30pin_01"].history) == 2


def test_sensor_fault_rules_flag_out_of_bounds_and_frozen_samples() -> None:
    engineer = FeatureEngineer()
    impossible = engineer.process(sample(rhum=120.0), now=100.0)
    assert impossible.fault_reason == "sensor_fault:out_of_bounds:rhum"
    assert engineer.update_anomaly_streak(
        "esp32:30pin_01", 0.0, impossible.fault_reason
    ) == 1

    frozen_engineer = FeatureEngineer()
    results = [
        frozen_engineer.process(sample(), now=100.0 + index * 3.0)
        for index in range(11)
    ]
    assert results[-2].fault_reason is None
    assert results[-1].fault_reason == "sensor_fault:frozen_values"


def test_model_anomaly_requires_three_consecutive_high_scores() -> None:
    engineer = FeatureEngineer()
    assert engineer.update_anomaly_streak("node-1", 0.66, None) == 0
    assert engineer.update_anomaly_streak("node-1", 0.90, None) == 0
    assert engineer.update_anomaly_streak("node-1", 0.66, None) == 1
    assert engineer.update_anomaly_streak("node-1", 0.65, None) == 0


def test_model_score_is_bounded_and_calibrated() -> None:
    class DecisionModel:
        def __init__(self, decision: float) -> None:
            self.decision = decision

        def decision_function(self, _scaled: object) -> list[float]:
            return [self.decision]

    class IdentityScaler:
        def transform(self, values: object) -> object:
            return values

    features = {name: 0.0 for name in FEATURE_NAMES}
    assert AnomalyModel(DecisionModel(0.0), IdentityScaler()).score(features) == 0.0
    severe_score = AnomalyModel(
        DecisionModel(-0.2), IdentityScaler()
    ).score(features)
    assert severe_score == 1.0
    assert AnomalyModel(DecisionModel(0.2), IdentityScaler()).score(features) == 0.0


def test_telemetry_auth_response_and_prometheus_labels(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(service, "API_TOKEN", "test-token")
    with TestClient(service.app) as client:
        client.app.state.model.score = lambda _features: 0.9
        payload = {
            "wifi": -52,
            "rco2": 850,
            "pm02": 8,
            "pm10": 12,
            "atmp": 22.4,
            "rhum": 45.1,
            "serialno": "esp32:30pin_01",
        }
        denied = client.post("/telemetry", json=payload)
        assert denied.status_code == 401

        headers = {"Authorization": "Bearer test-token"}
        responses = []
        for co2_value in (850, 851, 852):
            payload["rco2"] = co2_value
            responses.append(
                client.post("/telemetry", json=payload, headers=headers)
            )

        assert [response.json()["anomaly_flag"] for response in responses] == [
            0,
            0,
            1,
        ]
        assert set(responses[0].json()) == {
            "status",
            "anomaly_score",
            "anomaly_flag",
            "reason",
        }
        metric_output = client.get("/metrics").text
        assert (
            'airpulse_anomaly_flag{node="30pin_01",room="101"} 1.0'
            in metric_output
        )
