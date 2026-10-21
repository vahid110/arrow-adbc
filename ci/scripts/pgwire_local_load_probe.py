# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

"""Fixed isolated same-process load observation; never constructs a connection."""

import hashlib
import json
import os
import struct
import sys
import sysconfig
from pathlib import Path

MAX_METADATA = 1048576
MAX_IMAGES = 1024
MAX_COMMANDS = 4096
ARM64 = 16777228


class Invalid(ValueError):
    pass


def require(value):
    if not value:
        raise Invalid("invalid-load-observation")


def strict_json(data):
    def pairs(items):
        out = {}
        for key, value in items:
            require(key not in out)
            out[key] = value
        return out

    def invalid(_):
        raise Invalid("invalid-load-observation")

    return json.loads(
        data, object_pairs_hook=pairs, parse_float=invalid, parse_constant=invalid
    )


def header_size(data):
    require(len(data) >= 32)
    magic, cpu, subtype, filetype, count, size, flags, reserved = struct.unpack_from(
        "<8I", data
    )
    require(
        magic == 0xFEEDFACF
        and cpu == ARM64
        and 0 < count <= MAX_COMMANDS
        and 8 * count <= size <= MAX_METADATA
    )
    return size


def parse_macho(data):
    """Bounded thin64le ARM64 facts only; no dependency resolution."""
    size = header_size(data)
    require(len(data) >= 32 + size)
    _, cpu, subtype, filetype, count, _, flags, _ = struct.unpack_from("<8I", data)
    commands = memoryview(data)[32 : 32 + size]
    position = 0
    uuid = None
    deployment = []
    dylibs = []
    rpaths = []
    other = []

    def name(block, offset, minimum):
        require(minimum <= offset < len(block))
        value = bytes(block[offset:])
        end = value.find(b"\0")
        require(0 <= end <= 16384)
        return value[:end].decode("utf-8", "strict")

    for _ in range(count):
        require(position + 8 <= len(commands))
        command, length = struct.unpack_from("<II", commands, position)
        require(length >= 8 and length % 8 == 0 and position + length <= len(commands))
        block = commands[position : position + length]
        if command == 0x1B:
            require(length == 24 and uuid is None)
            uuid = bytes(block[8:24]).hex()
            require(uuid != "0" * 32)
        elif command == 0x32:
            require(length >= 24)
            platform, minimum, sdk, tools = struct.unpack_from("<4I", block, 8)
            require(length == 24 + tools * 8 and tools <= MAX_COMMANDS)
            require(all(value["platform"] != platform for value in deployment))
            tool_facts = [
                dict(
                    zip(
                        ("tool", "version"),
                        struct.unpack_from("<2I", block, 24 + index * 8),
                    )
                )
                for index in range(tools)
            ]
            require(len({value["tool"] for value in tool_facts}) == tools)
            deployment.append(
                {
                    "platform": platform,
                    "minos_packed": minimum,
                    "sdk_packed": sdk,
                    "tools": tool_facts,
                }
            )
        elif command in (0xD, 0xC, 0x80000018, 0x8000001F, 0x80000023):
            require(length >= 24)
            offset, timestamp, current, compatible = struct.unpack_from("<4I", block, 8)
            dylibs.append(
                {
                    "command": command,
                    "name": name(block, offset, 24),
                    "timestamp": timestamp,
                    "current": current,
                    "compatible": compatible,
                    "resolution": "unresolved",
                }
            )
        elif command == 0x8000001C:
            require(length >= 12)
            rpaths.append(name(block, struct.unpack_from("<I", block, 8)[0], 12))
        else:
            other.append({"command": command, "bytes": length})
        position += length
    require(position == len(commands) and uuid is not None)
    return {
        "cpu": cpu,
        "subtype": subtype,
        "filetype": filetype,
        "flags": flags,
        "uuid": uuid,
        "deployment": deployment,
        "dylibs": dylibs,
        "rpaths": rpaths,
        "other_commands": other,
    }


def file_fact(path):
    path = Path(path)
    require(path.is_absolute() and path.is_file() and not path.is_symlink())
    before = path.stat()
    require(before.st_size <= 67108864)
    data = path.read_bytes()
    after = path.stat()
    require(
        (
            before.st_dev,
            before.st_ino,
            before.st_size,
            before.st_mtime_ns,
            before.st_ctime_ns,
        )
        == (
            after.st_dev,
            after.st_ino,
            after.st_size,
            after.st_mtime_ns,
            after.st_ctime_ns,
        )
        and len(data) == before.st_size
    )
    return {
        "path": str(path),
        "sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data),
    }


def preimport(site):
    import importlib.machinery

    for package in ("adbc_driver_manager", "adbc_driver_redshift"):
        require(
            not any(
                key == package or key.startswith(package + ".") for key in sys.modules
            )
        )
        root = site / package
        require(root.is_dir() and not root.is_symlink())
        for path in root.rglob("*"):
            require(
                not path.is_symlink()
                and path.name != "__pycache__"
                and path.suffix not in (".pyc", ".pyo")
            )
        spec = importlib.machinery.PathFinder.find_spec(package)
        require(
            spec is not None
            and spec.origin is not None
            and Path(spec.origin) == root / "__init__.py"
            and list(spec.submodule_search_locations or []) == [str(root)]
        )


def observe(request):
    # All observation imports precede the stable dyld snapshot.
    import ctypes
    import importlib.metadata

    require(sys.platform == "darwin" and os.uname().machine == "arm64")
    require(
        type(request) is dict
        and set(request)
        == {"version", "site", "c_library", "wheel_library", "manager_native"}
    )
    require(type(request["version"]) is int and request["version"] == 1)
    for key in ("site", "c_library", "wheel_library", "manager_native"):
        value = request[key]
        require(
            type(value) is str
            and len(value) <= 16384
            and Path(value).is_absolute()
            and str(Path(value).resolve(strict=True)) == value
        )
    site = Path(request["site"])
    require(
        request["wheel_library"]
        == str(site / "adbc_driver_redshift/libadbc_driver_redshift.so")
    )
    preimport(site)
    import adbc_driver_manager
    import adbc_driver_manager._lib
    import adbc_driver_redshift

    actual = {
        "manager": Path(adbc_driver_manager.__file__),
        "manager_native": Path(adbc_driver_manager._lib.__file__),
        "redshift": Path(adbc_driver_redshift.__file__),
    }
    require(
        actual
        == {
            "manager": site / "adbc_driver_manager/__init__.py",
            "manager_native": Path(request["manager_native"]),
            "redshift": site / "adbc_driver_redshift/__init__.py",
        }
    )
    for distribution in ("adbc-driver-manager", "adbc-driver-redshift"):
        require(
            Path(importlib.metadata.distribution(distribution).locate_file("")) == site
        )
    handles = [ctypes.CDLL(request[key]) for key in ("c_library", "wheel_library")]
    for handle in handles:
        require(getattr(handle, "AdbcDriverRedshiftInit", None) is not None)
    # Resolving a symbol is not calling the initializer or creating a database.
    process = ctypes.CDLL(None)
    query = process.sysctlbyname
    query.argtypes = [
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.c_void_p,
        ctypes.c_size_t,
    ]
    query.restype = ctypes.c_int

    def os_fact(key):
        buffer = ctypes.create_string_buffer(256)
        size = ctypes.c_size_t(256)
        require(
            query(key, buffer, ctypes.byref(size), None, 0) == 0
            and 1 < size.value <= 256
        )
        data = bytes(buffer[: size.value])
        require(data[-1:] == b"\0" and b"\0" not in data[:-1])
        return data[:-1].decode("ascii")

    os_version, os_build = os_fact(b"kern.osproductversion"), os_fact(b"kern.osversion")
    origins = {key: file_fact(value) for key, value in actual.items()}
    for key in ("c_library", "wheel_library"):
        origins[key] = file_fact(request[key])
    abi = sysconfig.get_config_var("SOABI")
    count = process._dyld_image_count
    count.restype = ctypes.c_uint32
    image_name = process._dyld_get_image_name
    image_name.argtypes, image_name.restype = [ctypes.c_uint32], ctypes.c_char_p
    header = process._dyld_get_image_header
    header.argtypes, header.restype = [ctypes.c_uint32], ctypes.c_void_p
    total = count()
    require(0 < total <= MAX_IMAGES)
    images = []
    for index in range(total):
        raw, address = image_name(index), header(index)
        require(raw and address)
        path = raw.decode("utf-8", "strict")
        require(len(path) <= 16384 and Path(path).is_absolute())
        start = ctypes.string_at(address, 32)
        size = header_size(start)
        facts = parse_macho(start + ctypes.string_at(address + 32, size))
        if path.startswith(("/usr/lib/", "/System/Library/")):
            facts = {
                key: facts[key]
                for key in ("cpu", "subtype", "filetype", "flags", "uuid", "deployment")
            }
        images.append({"index": index, "path": path, **facts})
    require(count() == total and len(handles) == 2)
    return {
        "version": 1,
        "kind": "adbc-same-host-load-metadata",
        "scope": "named-load-only",
        "system": "Darwin",
        "machine": "arm64",
        "os_version": os_version,
        "os_build": os_build,
        "python": sys.version,
        "abi": abi,
        "executable": sys.executable,
        "origins": origins,
        "images": images,
        "connection_created": False,
        "broader_closure": "unknown",
    }


def main():
    try:
        import fcntl
        import stat

        require(
            sys.flags.isolated
            and sys.flags.dont_write_bytecode
            and sys.pycache_prefix is None
            and len(sys.argv) == 3
        )
        require(len(sys.argv[2]) <= 131072)
        fd = int(sys.argv[1])
        require(str(fd) == sys.argv[1] and fd > 2)
        info = os.fstat(fd)
        require(
            fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE in (os.O_WRONLY, os.O_RDWR)
        )
        require(
            stat.S_ISREG(info.st_mode)
            and info.st_uid == os.geteuid()
            and stat.S_IMODE(info.st_mode) == 0o600
            and info.st_nlink == 1
            and info.st_size == 0
        )
        require(os.lseek(fd, 0, os.SEEK_CUR) == 0)
        value = observe(strict_json(sys.argv[2]))
        data = json.dumps(
            value, sort_keys=True, separators=(",", ":"), allow_nan=False
        ).encode()
        require(len(data) <= MAX_METADATA)
        view = memoryview(data)
        while view:
            written = os.write(fd, view)
            require(written > 0)
            view = view[written:]
        os.fsync(fd)
        print(
            json.dumps(
                {
                    "version": 1,
                    "status": "load-only-observed",
                    "bytes": len(data),
                    "sha256": hashlib.sha256(data).hexdigest(),
                },
                separators=(",", ":"),
            )
        )
        return 0
    except BaseException:
        print("local load observation failed", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
