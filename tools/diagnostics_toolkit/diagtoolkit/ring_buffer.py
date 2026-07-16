"""Thread-safe accumulated text buffer."""

from __future__ import annotations

from dataclasses import dataclass, field
import threading


@dataclass
class TextRingBuffer:
    limit: int
    _text: str = ""
    _lock: threading.Lock = field(default_factory=threading.Lock)

    def append(self, text: str) -> None:
        if not text:
            return
        with self._lock:
            self._text += text
            if len(self._text) > self.limit:
                self._text = self._text[-self.limit :]

    def read(self, clear: bool = False, max_chars: int | None = None) -> str:
        with self._lock:
            if max_chars is None or max_chars <= 0:
                text = self._text
            else:
                text = self._text[-max_chars:]
            if clear:
                self._text = ""
            return text

    def clear(self) -> None:
        with self._lock:
            self._text = ""

    def size(self) -> int:
        with self._lock:
            return len(self._text)
