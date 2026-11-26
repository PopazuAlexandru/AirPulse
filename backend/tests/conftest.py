import sys
from pathlib import Path


AI_SERVICE_PATH = Path(__file__).resolve().parents[1] / "ai_service"
sys.path.insert(0, str(AI_SERVICE_PATH))
