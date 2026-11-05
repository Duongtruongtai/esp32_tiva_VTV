"""
Optional endpoint for the Robot Lab Tour Guide server (FastAPI).

The Tiva robot calls POST /api/v1/tts once to get fixed sentences (e.g. the goodbye
sentence) spoken with the robot's normal voice, then keeps the sound in its flash memory.
If this endpoint does not exist, the robot falls back to /api/v1/chat/text.

Add this to the existing FastAPI app and replace `synthesize_to_wav_bytes` with the
server's own Neural TTS function (the one /api/v1/voice/interact already uses).
"""

from typing import Optional

from fastapi import Response
from pydantic import BaseModel


class TtsRequest(BaseModel):
    text: str
    voice: Optional[str] = None
    format: Optional[str] = "wav"


@app.post("/api/v1/tts")  # noqa: F821  (`app` is the existing FastAPI instance)
async def text_to_speech(request: TtsRequest):
    # Must return 16-bit PCM WAV bytes (same as voice/interact with format='wav')
    wav_bytes = await synthesize_to_wav_bytes(request.text, voice=request.voice)  # noqa: F821
    return Response(content=wav_bytes, media_type="audio/wav")
