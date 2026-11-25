from __future__ import annotations

import os
from pathlib import Path

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import IsolationForest
from sklearn.preprocessing import StandardScaler

from features import FEATURE_NAMES


NORMAL_DECISION_CEILING = 0.0
SEVERE_DECISION_FLOOR = -0.20


class AnomalyModel:
    def __init__(self, model: IsolationForest, scaler: StandardScaler) -> None:
        self.model = model
        self.scaler = scaler

    @classmethod
    def load_or_train(cls) -> AnomalyModel:
        model_dir = Path(
            os.environ.get(
                "MODEL_DIR", str(Path(__file__).parent / "artifacts")
            )
        )
        model_path = model_dir / "model.joblib"
        scaler_path = model_dir / "scaler.joblib"
        if not model_path.is_file() or not scaler_path.is_file():
            from train_baseline import train_baseline

            train_baseline(model_dir)
        return cls(joblib.load(model_path), joblib.load(scaler_path))

    def score(self, features: dict[str, float]) -> float:
        vector = pd.DataFrame(
            [{name: features[name] for name in FEATURE_NAMES}],
            columns=FEATURE_NAMES,
        )
        scaled = self.scaler.transform(vector)
        decision = float(self.model.decision_function(scaled)[0])
        normalized = (NORMAL_DECISION_CEILING - decision) / (
            NORMAL_DECISION_CEILING - SEVERE_DECISION_FLOOR
        )
        return float(np.clip(normalized, 0.0, 1.0))
