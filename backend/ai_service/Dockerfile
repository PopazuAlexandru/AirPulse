FROM python:3.11-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    MODEL_DIR=/app/artifacts

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir --upgrade pip \
    && pip install --no-cache-dir -r requirements.txt

COPY app.py features.py model.py train_baseline.py ./
RUN mkdir -p "${MODEL_DIR}" \
    && python train_baseline.py --output-dir "${MODEL_DIR}" \
    && useradd --create-home --uid 10001 app \
    && chown -R app:app /app

USER app
EXPOSE 8000

CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8000"]
