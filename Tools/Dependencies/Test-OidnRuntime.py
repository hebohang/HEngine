"""Execute the shipped CPU denoiser in an isolated process; no SDK/compiler needed."""
import argparse
import ctypes as c
import json
from pathlib import Path


def verify_embree(root):
    """Exercise the Editor's other oneTBB consumer in the same process."""
    lib = c.CDLL(str(root / 'embree4.dll'), winmode=0x1100)
    ptr = c.c_void_p
    signatures = {
        'rtcNewDevice': (ptr, [c.c_char_p]), 'rtcReleaseDevice': (None, [ptr]),
        'rtcNewScene': (ptr, [ptr]), 'rtcReleaseScene': (None, [ptr]),
        'rtcNewGeometry': (ptr, [ptr, c.c_int]), 'rtcReleaseGeometry': (None, [ptr]),
        'rtcSetNewGeometryBuffer': (ptr, [ptr, c.c_int, c.c_uint, c.c_int, c.c_size_t, c.c_size_t]),
        'rtcCommitGeometry': (None, [ptr]), 'rtcCommitScene': (None, [ptr]),
        'rtcAttachGeometry': (c.c_uint, [ptr, ptr]), 'rtcGetDeviceError': (c.c_int, [ptr]),
    }
    for name, (result, args) in signatures.items():
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = result, args
    device = lib.rtcNewDevice(b'threads=4')
    assert device, 'Embree device creation failed'
    scene = geometry = None
    try:
        scene = lib.rtcNewScene(device)
        geometry = lib.rtcNewGeometry(device, 0)
        assert scene and geometry
        vertices = lib.rtcSetNewGeometryBuffer(geometry, 1, 0, 0x9003, 12, 3)
        indices = lib.rtcSetNewGeometryBuffer(geometry, 0, 0, 0x5003, 12, 4096)
        assert vertices and indices
        c.memmove(vertices, (c.c_float * 9)(0, 0, 0, 1, 0, 0, 0, 1, 0), 36)
        c.memmove(indices, (c.c_uint * (4096 * 3))(*([0, 1, 2] * 4096)), 4096 * 12)
        lib.rtcCommitGeometry(geometry)
        lib.rtcAttachGeometry(scene, geometry)
        lib.rtcCommitScene(scene)
        assert lib.rtcGetDeviceError(device) == 0, 'Embree scene build failed with shared oneTBB'
    finally:
        if geometry: lib.rtcReleaseGeometry(geometry)
        if scene: lib.rtcReleaseScene(scene)
        lib.rtcReleaseDevice(device)


def verify(root):
    root = root.resolve()
    plugin = root / 'Plugins/OfflineReferenceRenderer'
    # Load the exact shared runtime selected by the host, without PATH/CWD search.
    tbb = c.WinDLL(str(root / 'tbb12.dll'), winmode=0x1100)
    cpu = c.WinDLL(str(plugin / 'OpenImageDenoise_device_cpu.dll'), winmode=0x1100)
    lib = c.CDLL(str(plugin / 'OpenImageDenoise.dll'), winmode=0x1100)
    ptr = c.c_void_p
    signatures = {
        'oidnNewDevice': (ptr, [c.c_int]),
        'oidnGetDeviceInt': (c.c_int, [ptr, c.c_char_p]),
        'oidnSetDeviceInt': (None, [ptr, c.c_char_p, c.c_int]),
        'oidnSetDeviceBool': (None, [ptr, c.c_char_p, c.c_bool]),
        'oidnCommitDevice': (None, [ptr]),
        'oidnGetDeviceError': (c.c_int, [ptr, c.POINTER(c.c_char_p)]),
        'oidnNewFilter': (ptr, [ptr, c.c_char_p]),
        'oidnSetFilterBool': (None, [ptr, c.c_char_p, c.c_bool]),
        'oidnSetSharedFilterImage': (None, [ptr, c.c_char_p, ptr, c.c_int] + [c.c_size_t] * 5),
        'oidnCommitFilter': (None, [ptr]),
        'oidnExecuteFilter': (None, [ptr]),
        'oidnReleaseFilter': (None, [ptr]),
        'oidnReleaseDevice': (None, [ptr]),
    }
    for name, (result, args) in signatures.items():
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = result, args
    device = lib.oidnNewDevice(1)  # OIDN_DEVICE_TYPE_CPU
    filter_ = None

    def check():
        message = c.c_char_p()
        error = lib.oidnGetDeviceError(device, c.byref(message))
        if error:
            raise RuntimeError(message.value.decode('utf-8', 'replace') if message.value else f'OIDN error {error}')

    try:
        check()
        if not device:
            raise RuntimeError('OIDN CPU device unavailable')
        version = lib.oidnGetDeviceInt(device, b'version')
        assert version == 20501, version
        lib.oidnSetDeviceInt(device, b'numThreads', 4)
        lib.oidnSetDeviceBool(device, b'setAffinity', False)
        lib.oidnCommitDevice(device)
        check()
        filter_ = lib.oidnNewFilter(device, b'RT')
        check()
        assert filter_
        width = height = 32
        size = width * height * 4
        color = (c.c_float * size)(*([1.0] * size))
        albedo = (c.c_float * size)(*([1.0] * size))
        normal = (c.c_float * size)()
        output = (c.c_float * size)()
        seed = 123
        for pixel in range(width * height):
            seed = (seed * 1664525 + 1013904223) & 0xffffffff
            noise = ((seed & 65535) / 65535 - .5) * .8
            for channel in range(3):
                color[pixel * 4 + channel] += noise
            normal[pixel * 4 + 2] = 1.0
        original = list(color)
        for name, data in ((b'color', color), (b'albedo', albedo), (b'normal', normal), (b'output', output)):
            lib.oidnSetSharedFilterImage(filter_, name, data, 3, width, height, 0, 16, 0)  # FLOAT3
        lib.oidnSetFilterBool(filter_, b'hdr', True)
        lib.oidnSetFilterBool(filter_, b'cleanAux', False)
        lib.oidnCommitFilter(filter_)
        check()
        lib.oidnExecuteFilter(filter_)
        check()
        raw = sum((color[p * 4 + ch] - 1) ** 2 for p in range(width * height) for ch in range(3))
        denoised = sum((output[p * 4 + ch] - 1) ** 2 for p in range(width * height) for ch in range(3))
        assert list(color) == original, 'Denoiser modified input'
        assert denoised < raw, (raw, denoised)
        verify_embree(root)
        return dict(evidence='test-layer', status='PASS', version=version, backend='CPU', embreeSharedTbb='PASS',
                    rawSquaredError=raw, denoisedSquaredError=denoised)
    finally:
        if filter_:
            lib.oidnReleaseFilter(filter_)
        if device:
            lib.oidnReleaseDevice(device)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--engine', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.engine)))
