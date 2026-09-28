"""Standalone correctness-first simulation interface over Fathom's C ABI."""
from __future__ import annotations

from dataclasses import dataclass
import json
import os
from typing import TYPE_CHECKING
from . import _native
from ._native import FathomError

if TYPE_CHECKING:
    import pyarrow

__all__ = ["Model", "Experiment", "run", "lint", "FathomError"]


def _json_bytes(value: str | bytes | dict) -> bytes:
    if isinstance(value, bytes):
        return value
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, dict):
        return json.dumps(value, ensure_ascii=True, allow_nan=False).encode("utf-8")
    raise TypeError("JSON must be str, bytes, or dict")


class Model:
    """An immutable, validated model and its bound-data snapshots."""
    __slots__ = ("_handle",)

    def __init__(self):
        raise TypeError("use Model.from_json()")

    @classmethod
    def from_json(cls, source: str | bytes | dict, *, base_directory=None) -> Model:
        """Load JSON; data bindings require an absolute base_directory."""
        base = None if base_directory is None else os.fsdecode(os.fspath(base_directory))
        handle = _native.Model.from_json(_json_bytes(source), base)
        model = object.__new__(cls)
        model._handle = handle
        return model


@dataclass(frozen=True, init=False)
class Experiment:
    """A copied experiment JSON specification, validated against the model at run."""
    _json: bytes

    def __init__(self, source: str | bytes | dict):
        object.__setattr__(self, "_json", _json_bytes(source))


class _Stream:
    def __init__(self, capsule):
        self._capsule = capsule

    def __arrow_c_stream__(self, requested_schema=None):
        if requested_schema is not None:
            raise ValueError("schema conversion is not supported")
        if self._capsule is None:
            raise RuntimeError("stream ownership has already been transferred")
        capsule, self._capsule = self._capsule, None
        return capsule


def run(model: Model, experiment: Experiment | None = None, *, threads: int = 1,
        seed: int | None = None, provenance: bool = False, progress=None) -> pyarrow.Table:
    """Return an owned table; provenance=True adds schema-0.2 lineage and a manifest."""
    if not isinstance(model, Model):
        raise TypeError("model must be a Model")
    if experiment is not None and not isinstance(experiment, Experiment):
        raise TypeError("experiment must be an Experiment or None")
    if type(threads) is not int or not 1 <= threads <= 256:
        raise ValueError("threads must be an integer in [1, 256]")
    if seed is not None and (type(seed) is not int or not 0 <= seed <= (1 << 64) - 1):
        raise ValueError("seed must be an integer in [0, 2**64 - 1] or None")
    if type(provenance) is not bool:
        raise TypeError("provenance must be a bool")
    if progress is not None and not callable(progress):
        raise TypeError("progress must be callable or None")
    import pyarrow as pa
    capsule = _native.run_stream(model._handle, None if experiment is None else experiment._json,
                                 threads, seed, provenance, progress)
    with pa.RecordBatchReader.from_stream(_Stream(capsule)) as reader:
        return reader.read_all()


def lint(source: str | bytes | dict, *, base_directory=None) -> dict:
    """Validate using the shared loader. This does not run behavioral M7 checks."""
    try:
        Model.from_json(source, base_directory=base_directory)
    except FathomError as error:
        if error.status not in (1, 2):
            raise
        return {"verdict": "fail", "diagnostics": [{
            "code": error.code, "pointer": error.pointer, "message": str(error),
            "truncated": error.truncated}]}
    return {"verdict": "pass", "diagnostics": []}
