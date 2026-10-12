"""Production validation using the Windows resource APIs; no SDK helper scripts."""

import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET


DIAGNOSTIC_COMMANDS = (
    "--self-test", "--test-child", "--component-benchmark", "--benchmark",
    "--ui-benchmark", "--startup-inventory", "--screenshots",
)
DIAGNOSTIC_MARKERS = (
    "startup_fixture_created", "cachedSort", "keyboardPassed", "tabSpamPassed",
    "columnResize", "scrollbar-rebuild-no-intermediate-paints",
    "scrollbar-rebuild-explicit-capture", "caption-immediate-theme-pixels",
    "--tab-spam", "--self-test", "--component-benchmark", "--ui-benchmark",
    "--screenshots", "TaskManagerNativeTests",
)


def require_windows():
    if os.name != "nt":
        raise ValueError("Native build and executable resource checks require Windows x64.")


def process_options():
    # subprocess performs argument quoting; never use a shell for executable paths.
    return {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}


def read_manifest(executable):
    """Read RT_MANIFEST #1 as data, without executing any code from the file."""
    require_windows()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    kernel.LoadLibraryExW.restype = wintypes.HMODULE
    kernel.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
    kernel.FindResourceW.restype = wintypes.HRSRC
    kernel.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HRSRC]
    kernel.SizeofResource.restype = wintypes.DWORD
    kernel.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HRSRC]
    kernel.LoadResource.restype = wintypes.HGLOBAL
    kernel.LockResource.argtypes = [wintypes.HGLOBAL]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = [wintypes.HMODULE]
    kernel.FreeLibrary.restype = wintypes.BOOL
    # LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE.
    module = kernel.LoadLibraryExW(str(Path(executable).resolve()), None, 0x60)
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        resource = kernel.FindResourceW(module, 1, 24)  # MAKEINTRESOURCE(1), RT_MANIFEST
        if not resource:
            raise ctypes.WinError(ctypes.get_last_error())
        size = kernel.SizeofResource(module, resource)
        loaded = kernel.LoadResource(module, resource)
        pointer = kernel.LockResource(loaded) if loaded else None
        if not size or not pointer:
            raise ctypes.WinError(ctypes.get_last_error())
        return ctypes.string_at(pointer, size)
    finally:
        kernel.FreeLibrary(module)


def executable_versions(executable):
    """Return both numeric versions and the resource's displayed version strings."""
    require_windows()
    version = ctypes.WinDLL("version", use_last_error=True)
    version.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    version.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    version.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    version.GetFileVersionInfoW.restype = wintypes.BOOL
    version.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR,
                                     ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    version.VerQueryValueW.restype = wintypes.BOOL
    path = str(Path(executable).resolve())
    size = version.GetFileVersionInfoSizeW(path, None)
    if not size:
        raise ctypes.WinError(ctypes.get_last_error())
    buffer = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(path, 0, size, buffer):
        raise ctypes.WinError(ctypes.get_last_error())

    def query(name):
        pointer, length = ctypes.c_void_p(), wintypes.UINT()
        if not version.VerQueryValueW(buffer, name, ctypes.byref(pointer), ctypes.byref(length)) \
                or not pointer.value or not length.value:
            raise ValueError(f"Executable is missing version resource {name}.")
        return pointer, length.value

    pointer, length = query("\\")
    if length < 52:
        raise ValueError("Executable fixed version resource is truncated.")
    fields = struct.unpack("<13I", ctypes.string_at(pointer, 52))
    if fields[0] != 0xFEEF04BD:
        raise ValueError("Executable fixed version resource has an invalid signature.")
    versions = []
    for high, low in ((fields[2], fields[3]), (fields[4], fields[5])):
        versions.append(f"{high >> 16}.{high & 0xffff}.{low >> 16}.{low & 0xffff}")
    pointer, length = query("\\VarFileInfo\\Translation")
    if length % 4:
        raise ValueError("Executable version translations are malformed.")
    for language, codepage in struct.iter_unpack("<HH", ctypes.string_at(pointer, length)):
        for name in ("FileVersion", "ProductVersion"):
            pointer, characters = query(f"\\StringFileInfo\\{language:04x}{codepage:04x}\\{name}")
            versions.append(ctypes.wstring_at(pointer, characters).rstrip("\0"))
    return versions


def validate_manifest(contents):
    try:
        root = ET.fromstring(contents)
    except ET.ParseError as error:
        raise ValueError("Production execution manifest is invalid XML.") from error
    execution = root.findall(".//{*}requestedExecutionLevel")
    if len(execution) != 1 or execution[0].get("level") != "requireAdministrator" \
            or execution[0].get("uiAccess") != "false":
        raise ValueError("Production must require administrator elevation without UI-access privileges.")
    return root


def check_markers(contents):
    for marker in DIAGNOSTIC_MARKERS:
        if marker.encode("ascii") in contents or marker.encode("utf-16-le") in contents:
            raise ValueError(f"Production executable still contains diagnostic marker: {marker}")


def is_elevated():
    require_windows()
    shell = ctypes.WinDLL("shell32", use_last_error=True)
    shell.IsUserAnAdmin.argtypes = []
    shell.IsUserAnAdmin.restype = wintypes.BOOL
    return bool(shell.IsUserAnAdmin())


def check_runtime(executable, elevated):
    if not elevated:
        try:
            subprocess.run([str(executable), "--self-test"], timeout=10, check=False,
                           **process_options())
        except OSError as error:
            if getattr(error, "winerror", None) == 740:
                print("Administrator manifest verified; Windows refused unelevated launch (740). "
                      "Runtime command rejection requires an elevated terminal.")
                return
            raise
        raise ValueError("Production executable started without administrator elevation.")
    # The report path does not exist before launch. Check it after each invocation.
    with tempfile.TemporaryDirectory(prefix="TaskManagerProduction-") as directory:
        destination = Path(directory) / "diagnostic-output"
        for command in DIAGNOSTIC_COMMANDS:
            process = subprocess.run([str(executable), command, str(destination),
                                      "--output", str(destination)], timeout=10,
                                     check=False, **process_options())
            if process.returncode != 2:
                raise ValueError(f"Production did not reject {command} (exit {process.returncode}).")
            if destination.exists():
                raise ValueError(f"Production executable wrote diagnostic output for {command}.")


def check_production(executable, runtime=True):
    executable = Path(executable).resolve(strict=True)
    validate_manifest(read_manifest(executable))
    check_markers(executable.read_bytes())
    if runtime:
        check_runtime(executable, is_elevated())
    print("Production exclusion checks passed; diagnostic markers absent and administrator manifest verified.")
