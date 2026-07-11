from __future__ import annotations

import asyncio
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from .serial_backend import DEFAULT_SERIAL_BAUD, DEFAULT_SERIAL_PORT, SerialBackend


STATIC_DIR = Path(__file__).with_name("static")


class SerialConnectRequest(BaseModel):
    port: str = Field(default=DEFAULT_SERIAL_PORT, min_length=1)
    baud: int = Field(default=DEFAULT_SERIAL_BAUD, ge=1, le=3000000)


class SerialCommandRequest(BaseModel):
    command: str = Field(min_length=1)
    args: list[str] = Field(default_factory=list)


class SerialRawRequest(BaseModel):
    line: str = Field(min_length=1, max_length=160)


class WebSocketManager:
    def __init__(self) -> None:
        self._clients: set[WebSocket] = set()
        self._lock = asyncio.Lock()

    async def connect(self, websocket: WebSocket) -> None:
        await websocket.accept()
        async with self._lock:
            self._clients.add(websocket)

    async def disconnect(self, websocket: WebSocket) -> None:
        async with self._lock:
            self._clients.discard(websocket)

    async def broadcast(self, payload: dict[str, Any]) -> None:
        async with self._lock:
            clients = list(self._clients)
        for websocket in clients:
            try:
                await websocket.send_json(payload)
            except Exception:
                await self.disconnect(websocket)


serial_backend = SerialBackend()
manager = WebSocketManager()
# Give the copied FastAPI app its own title so browser/API users can distinguish it from the broad debug driver.
app = FastAPI(title="Conveyor PID Driver")
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.on_event("startup")
async def startup() -> None:
    asyncio.create_task(_event_pump())


@app.on_event("shutdown")
async def shutdown() -> None:
    serial_backend.disconnect()


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html", headers={"Cache-Control": "no-store"})


@app.get("/health")
async def health() -> dict[str, Any]:
    return {"ok": True, "serial_snapshot": serial_backend.snapshot_dict()}


@app.get("/api/serial/snapshot")
async def serial_snapshot() -> dict[str, Any]:
    return serial_backend.snapshot_dict()


@app.post("/api/serial/connect")
async def serial_connect(request: SerialConnectRequest) -> dict[str, Any]:
    try:
        serial_backend.connect(request.port, request.baud)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except RuntimeError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    await _broadcast_serial_snapshot()
    return serial_backend.snapshot_dict()


@app.post("/api/serial/disconnect")
async def serial_disconnect() -> dict[str, Any]:
    serial_backend.disconnect()
    await _broadcast_serial_snapshot()
    return serial_backend.snapshot_dict()


@app.post("/api/serial/command")
async def serial_command(request: SerialCommandRequest) -> dict[str, Any]:
    try:
        line = serial_backend.command(request.command, request.args)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except RuntimeError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    await _broadcast_serial_snapshot()
    return {"ok": True, "line": line, "serial_snapshot": serial_backend.snapshot_dict()}


@app.post("/api/serial/raw")
async def serial_raw(request: SerialRawRequest) -> dict[str, Any]:
    try:
        line = serial_backend.raw(request.line)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except RuntimeError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    await _broadcast_serial_snapshot()
    return {"ok": True, "line": line, "serial_snapshot": serial_backend.snapshot_dict()}


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket) -> None:
    await manager.connect(websocket)
    await websocket.send_json({"type": "serial_snapshot", "serial_snapshot": serial_backend.snapshot_dict()})
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        await manager.disconnect(websocket)


async def _event_pump() -> None:
    while True:
        while not serial_backend.events.empty():
            event = serial_backend.events.get()
            await manager.broadcast(
                {
                    "type": "serial_event",
                    "event": event.as_dict(),
                    "serial_snapshot": serial_backend.snapshot_dict(),
                }
            )
        await asyncio.sleep(0.05)


async def _broadcast_serial_snapshot() -> None:
    await manager.broadcast({"type": "serial_snapshot", "serial_snapshot": serial_backend.snapshot_dict()})
