"""Opt-in mstx/msproftx ranges for UCM user-space phases.

Markers are only recorded when msprof runs with ``--msproftx=on``; outside a
profiling session the mstx calls are cheap no-ops. Set ``UCM_MSTX=0`` to
disable instrumentation. Marker names are prefixed with ``ucm.`` so they are
easy to filter from other mstx users.
"""

from __future__ import annotations

from contextlib import contextmanager
from functools import wraps
import os

from ucm.logger import init_logger

logger = init_logger(__name__)

_ENABLED = os.getenv("UCM_MSTX", "1").lower() not in ("0", "false", "off", "no")
_mstx = None
_mstx_checked = False


def _get_mstx():
    global _mstx, _mstx_checked
    if _mstx_checked:
        return _mstx
    _mstx_checked = True
    if not _ENABLED:
        return None
    # The standalone mstx module records both host marks and stream ranges.
    # The torch.npu.mstx wrapper only records host marks here, which msprof
    # does not export on this CANN build, so try native mstx first.
    try:
        import mstx as candidate

        _mstx = candidate
        return _mstx
    except Exception as error:  # noqa: BLE001 - tracing must never break serving
        logger.debug("native mstx unavailable: %s", error)
    try:
        import torch

        _mstx = getattr(getattr(torch, "npu", None), "mstx", None)
    except Exception:
        _mstx = None
    return _mstx


def _stream_handle():
    try:
        import torch

        if not torch.npu.is_initialized():
            return None
        stream = torch.npu.current_stream()
        return getattr(stream, "npu_stream", None)
    except Exception:
        return None


@contextmanager
def ucm_range(name: str, domain: str | None = None):
    """Emit one nested mstx range around the wrapped block."""
    del domain  # native mstx uses the default domain; names are ucm.*
    mstx = _get_mstx()
    if mstx is None:
        yield
        return
    stream = _stream_handle()
    if stream is None:
        # Scheduler-side context has no NPU stream; a point mark still shows
        # up in the timeline without initializing an NPU device.
        try:
            mstx.mark(name)
        except Exception as error:  # noqa: BLE001
            logger.debug("mstx mark(%s) failed: %s", name, error)
        yield
        return
    range_id = None
    try:
        range_id = mstx.range_start(name, stream)
    except Exception as error:  # noqa: BLE001
        logger.debug("mstx range_start(%s) failed: %s", name, error)
        yield
        return
    try:
        yield
    finally:
        try:
            mstx.range_end(range_id)
        except Exception as error:  # noqa: BLE001
            logger.debug("mstx range_end(%s) failed: %s", name, error)


def traced(name: str, domain: str | None = None):
    """Decorator emitting an mstx range for a sync method call."""

    def decorator(func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            with ucm_range(name, domain):
                return func(*args, **kwargs)

        return wrapper

    return decorator


def attention_phase(forward_context) -> str:
    """Classify a step as prefill/decode from vllm-ascend attention metadata."""
    metadata = getattr(forward_context, "attn_metadata", None)
    if metadata is None:
        return "unknown"
    values = metadata.values() if isinstance(metadata, dict) else [metadata]
    phases = set()
    for item in values:
        state = getattr(item, "attn_state", None)
        name = getattr(state, "name", None) or str(state)
        if name in ("DecodeOnly", "SpecDecoding"):
            phases.add("decode")
        elif name == "ChunkedPrefill":
            phases.add("chunked_prefill")
        elif name in ("PrefillNoCache", "PrefillCacheHit"):
            phases.add("prefill")
    if not phases:
        return "unknown"
    if len(phases) == 1:
        return phases.pop()
    return "mixed"


def traced_phase(prefix: str):
    """Decorator appending .prefill/.decode/.chunked_prefill/.mixed to the name."""

    def decorator(func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            forward_context = kwargs.get("forward_context")
            if forward_context is None:
                for value in args:
                    if hasattr(value, "attn_metadata"):
                        forward_context = value
                        break
            phase = attention_phase(forward_context)
            with ucm_range(f"{prefix}.{phase}"):
                return func(*args, **kwargs)

        return wrapper

    return decorator

def mark(name: str, domain: str | None = None) -> None:
    """Emit a point-in-time mstx mark."""
    del domain
    mstx = _get_mstx()
    if mstx is None:
        return
    try:
        mstx.mark(name, _stream_handle())
    except Exception as error:  # noqa: BLE001
        logger.debug("mstx mark(%s) failed: %s", name, error)
