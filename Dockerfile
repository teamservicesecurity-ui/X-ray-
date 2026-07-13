FROM python:3.11-slim

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY . .

ENV BOT_TOKEN=your_token_here
ENV ADMIN_IDS=your_telegram_id

CMD ["python3", "bot.py"]
