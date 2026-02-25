# Errors and diagnostics

This page describes how errors are reported in Python and the C API.

## Python

Python bindings raise `RuntimeError` on failures.

Example:

```python
import sqzc3d

try:
    v = sqzc3d.read("missing.c3d")
except RuntimeError as e:
    print("failed:", e)
```

Feature discovery:

```python
import sqzc3d

features = sqzc3d.features()
print("features=", features)
```

If a build is missing a capability, APIs may fail with `sqzc3d_STATUS_NOT_IMPLEMENTED` (surfaced as an exception in Python).

## C API

Most APIs return a `sqzc3d_STATUS_*` code (see `sqzc3d_types.h`).

When a call fails, use:

- `sqzc3d_last_error(dec)` to get a human-readable message
- `sqzc3d_last_error(NULL)` for errors that happen before a decoder handle exists (e.g. open failure)

Example:

```c
sqzc3d_dec_t* dec = NULL;
int st = sqzc3d_open_file(&dec, path, &open_opt);
if (st != sqzc3d_STATUS_SUCCESS) {
  fprintf(stderr, "open failed: %s\n", sqzc3d_last_error(NULL));
  return 1;
}
```

Structured details (optional):

- `sqzc3d_last_error_detail(dec, &detail)`
