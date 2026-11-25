from __future__ import annotations

import argparse
from pathlib import Path

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import IsolationForest
from sklearn.preprocessing import StandardScaler

from features import EMA_WINDOW_SECONDS, FEATURE_NAMES


def generate_baseline(sample_count: int = 28800, seed: int = 42) -> pd.DataFrame:
    """Create three-second indoor measurements with realistic daily variation."""
    rng = np.random.default_rng(seed)
    sample_period_seconds = 3.0
    seconds = np.arange(sample_count, dtype=np.float64) * sample_period_seconds
    hour_of_day = (seconds / 3600.0) % 24.0
    occupied = np.clip(
        (np.sin(2.0 * np.pi * (hour_of_day - 8.0) / 24.0) + 1.0) / 2.0,
        0.0,
        1.0,
    )
    daily_cycle = np.sin(2.0 * np.pi * (hour_of_day - 7.0) / 24.0)

    co2 = (
        450.0
        + occupied * 420.0
        + 45.0 * daily_cycle
        + rng.normal(0.0, 1.5, sample_count)
    )
    temperature = (
        21.5
        + 1.8 * np.sin(2.0 * np.pi * (hour_of_day - 14.0) / 24.0)
        + rng.normal(0.0, 0.08, sample_count)
    )
    humidity = (
        46.0
        + 5.0 * np.sin(2.0 * np.pi * (hour_of_day + 2.0) / 24.0)
        + rng.normal(0.0, 0.25, sample_count)
    )
    pm25 = np.clip(rng.lognormal(mean=0.35, sigma=0.45, size=sample_count), 0.0, 8.0)
    pm10 = pm25 + rng.lognormal(mean=0.05, sigma=0.45, size=sample_count)
    delta_co2 = np.diff(co2, prepend=co2[0]) / sample_period_seconds
    ema_alpha = 2.0 * sample_period_seconds / (
        EMA_WINDOW_SECONDS + sample_period_seconds
    )
    temperature_ema = (
        pd.Series(temperature).ewm(alpha=ema_alpha, adjust=False).mean().to_numpy()
    )
    humidity_ema = (
        pd.Series(humidity).ewm(alpha=ema_alpha, adjust=False).mean().to_numpy()
    )

    return pd.DataFrame(
        {
            "rco2": co2,
            "d_co2_dt": delta_co2,
            "pm02": pm25,
            "pm10": pm10,
            "pm_ratio": pm25 / np.maximum(pm10, 1.0),
            "delta_temp": temperature - temperature_ema,
            "delta_hum": humidity - humidity_ema,
        },
        columns=FEATURE_NAMES,
    )


def train_baseline(output_dir: Path, force: bool = False) -> tuple[Path, Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    model_path = output_dir / "model.joblib"
    scaler_path = output_dir / "scaler.joblib"
    if not force and model_path.is_file() and scaler_path.is_file():
        print(f"Using existing baseline artifacts in {output_dir}")
        return model_path, scaler_path

    training_data = generate_baseline()
    scaler = StandardScaler()
    scaled_data = scaler.fit_transform(training_data)
    model = IsolationForest(
        n_estimators=200,
        contamination=0.02,
        max_samples="auto",
        random_state=42,
        n_jobs=1,
    )
    model.fit(scaled_data)
    joblib.dump(model, model_path, compress=3)
    joblib.dump(scaler, scaler_path, compress=3)
    print(f"Trained baseline model with {len(training_data)} samples in {output_dir}")
    return model_path, scaler_path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).parent / "artifacts",
        help="directory for model.joblib and scaler.joblib",
    )
    parser.add_argument(
        "--force", action="store_true", help="retrain even when artifacts exist"
    )
    arguments = parser.parse_args()
    train_baseline(arguments.output_dir, force=arguments.force)


if __name__ == "__main__":
    main()
