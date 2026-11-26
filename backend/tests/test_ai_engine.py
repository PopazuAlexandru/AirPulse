from __future__ import annotations

from collections.abc import Iterator

from fastapi.testclient import TestClient
import pytest

import app as service
from features import FEATURE_NAMES, FeatureEngineer, TelemetrySample
from model import AnomalyModel


def payload(**overrides: object) -> dict[str, object]:
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
    return values


@pytest.fixture
def client(monkeypatch: pytest.MonkeyPatch) -> Iterator[TestClient]:
    class BaselineModel:
        @staticmethod
        def score(_features: dict[str, float]) -> float:
            return 0.1

    monkeypatch.setattr(service, "API_TOKEN", "test-token")
    monkeypatch.setattr(
        AnomalyModel, "load_or_train", staticmethod(lambda: BaselineModel())
    )
    with TestClient(service.app) as test_client:
        yield test_client


def post_sample(
    client: TestClient, sample_payload: dict[str, object] | None = None
) -> dict[str, object]:
    response = client.post(
        "/telemetry",
        json=sample_payload or payload(),
        headers={"Authorization": "Bearer test-token"},
    )
    assert response.status_code == 200
    return response.json()


def test_baseline_payload_is_normal_and_metrics_are_exposed(
    client: TestClient,
) -> None:
    result = post_sample(client)

    assert result["status"] == "ok"
    assert result["anomaly_score"] == 0.1
    assert result["anomaly_flag"] == 0
    metrics = client.get("/metrics")
    assert metrics.status_code == 200
    assert (
        'airpulse_co2_ppm{node="30pin_01",room="101"} 850.0'
        in metrics.text
    )
    assert (
        'airpulse_anomaly_flag{node="30pin_01",room="101"} 0.0'
        in metrics.text
    )


def test_smoke_and_pm_spike_requires_three_smoothed_high_samples(
    client: TestClient,
) -> None:
    client.app.state.model.score = lambda features: (
        0.95 if features["pm02"] >= 100 else 0.1
    )
    post_sample(client)

    results = [
        post_sample(client, payload(pm02=120, pm10=150))
        for _ in range(4)
    ]

    assert [result["anomaly_flag"] for result in results] == [0, 0, 0, 1]
    assert results[-1]["reason"] == "critical_spike_confirmed"


def test_rapid_co2_accumulation_is_included_in_model_features(
    client: TestClient,
) -> None:
    scored_features: list[dict[str, float]] = []

    def score(features: dict[str, float]) -> float:
        scored_features.append(features)
        return 0.9 if features["d_co2_dt"] > 100 else 0.1

    client.app.state.model.score = score
    for co2 in (850, 880, 1200, 1700, 2300, 2900):
        result = post_sample(client, payload(rco2=co2))

    assert scored_features[2]["d_co2_dt"] > 100
    assert result["anomaly_flag"] == 1


def test_ema_smoothing_reduces_a_transient_spike(
    client: TestClient,
) -> None:
    scores = iter((0.2, 0.9, 0.2))
    client.app.state.model.score = lambda _features: next(scores)

    results = [post_sample(client) for _ in range(3)]

    assert [result["anomaly_score"] for result in results] == [0.2, 0.55, 0.38]
    assert [result["anomaly_flag"] for result in results] == [0, 0, 0]


def test_sensor_fault_is_flagged_immediately(client: TestClient) -> None:
    result = post_sample(client, payload(rhum=120.0))

    assert result["anomaly_flag"] == 1
    assert result["reason"] == "sensor_fault:out_of_bounds:rhum"


def test_feature_engineering_tracks_rate_ratio_and_ema_deltas() -> None:
    engineer = FeatureEngineer()
    first = TelemetrySample(**payload())
    second = TelemetrySample(**payload(rco2=880, atmp=23.0, rhum=47.0))

    first_result = engineer.process(first, now=100.0)
    second_result = engineer.process(second, now=103.0)

    assert first_result.fault_reason is None
    assert second_result.values["d_co2_dt"] == 10.0
    assert second_result.values["pm_ratio"] == 8.0 / 12.0
    assert second_result.values["delta_temp"] > 0
    assert second_result.values["delta_hum"] > 0
    assert len(engineer.devices["esp32:30pin_01"].history) == 2


def test_anomaly_streak_resets_when_scores_fall_below_threshold() -> None:
    engineer = FeatureEngineer()
    assert engineer.update_anomaly_streak("node-1", 0.66, None) == 0
    assert engineer.update_anomaly_streak("node-1", 0.90, None) == 0
    assert engineer.update_anomaly_streak("node-1", 0.66, None) == 1
    assert engineer.update_anomaly_streak("node-1", 0.65, None) == 0


def test_score_isolation_forest_normalization_is_calibrated() -> None:
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
