"""Analyze explicit linear-HDR transport ROIs; no automatic image-quality acceptance.

Examples (quote globs on Windows):
  python Tools/Agent/Analyze-SSGITransport.py --self-test --output self-test.json
  python Tools/Agent/Analyze-SSGITransport.py --input "run/raw-*.pfm" \
      --roi dark:820,390,970,610 --depth run/depth.raw --output dark.json

Guides accept one static file or one file per input, in the supplied order.
Coordinates are top-down, half-open pixel rectangles (or normalized with
--normalized-roi). Temporal statistics are fixed-pixel statistics: camera/object
motion requires externally aligned captures; they are not reprojection metrics.
Requires NumPy; PNG masks additionally require Pillow.
"""

import argparse
import glob
import json
import math
from pathlib import Path
import tempfile
from types import SimpleNamespace

import numpy as np


LUMA = np.array([0.2126, 0.7152, 0.0722])


def load_image(path):
    """Return top-down HWC without sanitizing or applying display exposure."""
    path = Path(path)
    if path.suffix.lower() == '.gz' and path.stem.lower().endswith('.pfm'):
        path = path.with_suffix('')
    if path.suffix.lower() == '.pfm':
        # Lossless archived sequences retain their logical PFM names in event
        # manifests; read the adjacent gzip directly without extracting copies.
        import gzip
        stream_source = path.open('rb') if path.exists() else gzip.open(str(path) + '.gz', 'rb')
        with stream_source as stream:
            def line():
                value = stream.readline()
                while value.startswith(b'#') or not value.strip():
                    if not value:
                        raise ValueError(f'Truncated PFM header: {path}')
                    value = stream.readline()
                return value.strip()
            magic = line()
            if magic not in (b'PF', b'Pf'):
                raise ValueError(f'Invalid PFM magic: {path}')
            width, height = map(int, line().split())
            scale = float(line())
            if width < 1 or height < 1 or not math.isfinite(scale) or scale == 0:
                raise ValueError(f'Invalid PFM dimensions/scale: {path}')
            channels = 3 if magic == b'PF' else 1
            data = np.frombuffer(stream.read(), dtype='<f4' if scale < 0 else '>f4')
            if data.size != width * height * channels:
                raise ValueError(f'PFM payload size mismatch: {path}')
            image = data.reshape(height, width, channels)[::-1].astype(np.float64)
            return image * abs(scale), {'format': 'PFM', 'alpha': 'discarded',
                                       'source_precision': 'unknown'}
    if path.suffix.lower() == '.png':
        from PIL import Image
        return np.asarray(Image.open(path).convert('L'))[..., None], {'format': 'mask PNG'}
    # EncodeRaw writes tightly packed little-endian HWC, top-down, preserving
    # float16/float32/uint32 components; staging rowPitch is NOT serialized.
    metadata = json.loads(Path(str(path) + '.json').read_text(encoding='utf-8-sig'))
    if metadata.get('schema') != 'hengine.texture-readback.raw.v1':
        raise ValueError(f'Unsupported raw schema: {path}')
    dtype = {'float16': '<f2', 'float32': '<f4', 'uint32': '<u4'}.get(metadata['dtype'])
    if (dtype is None or metadata['endianness'] != 'little' or
            metadata['layout'] != 'HWC' or metadata['row_order'] != 'top-down'):
        raise ValueError(f'Unsupported raw layout: {path}')
    shape = (metadata['height'], metadata['width'], metadata['channels'])
    if any(not isinstance(v, int) or v < 1 for v in shape):
        raise ValueError(f'Invalid raw shape: {path}')
    data = np.fromfile(path, dtype=dtype)
    if data.size != math.prod(shape):
        raise ValueError(f'Raw payload size mismatch: {path}')
    return data.reshape(shape).astype(np.float64), metadata


def summary(values):
    values = np.asarray(values)
    finite = values[np.isfinite(values)]
    return {'count': int(values.size), 'finite_count': int(finite.size),
            'nan': int(np.isnan(values).sum()),
            'positive_inf': int(np.isposinf(values).sum()),
            'negative_inf': int(np.isneginf(values).sum()),
            'negative_finite': int((finite < 0).sum()),
            'nonzero_finite': int(np.count_nonzero(finite)),
            'at_or_above_fp16_max': int((finite >= 65504).sum()),
            'at_fp16_max': int((finite == 65504).sum()),
            'mean': float(finite.mean()) if finite.size else None,
            'p50_p95_p99': np.percentile(finite, [50, 95, 99]).tolist() if finite.size else None}


def neighbors(image):
    for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
        yield np.roll(image, (dy, dx), axis=(0, 1))


def surface_mask(shape, guides, args):
    mask = np.ones(shape, dtype=bool)
    depth = guides.get('depth')
    if depth is not None:
        depth = depth[..., 0]
        mask &= np.isfinite(depth)
        mask &= ((depth >= 0) & (depth < 1)) if args.depth_kind == 'device' else depth > 0
        tolerance = (args.depth_edge if args.depth_kind == 'device' else
                     args.depth_edge * np.maximum(np.abs(depth), 0.001))
        for adjacent in neighbors(depth):
            mask &= np.isfinite(adjacent) & (np.abs(depth - adjacent) <= tolerance)
    normal = guides.get('normal')
    if normal is not None:
        if normal.shape[2] < 3:
            raise ValueError('Normal guide requires at least three signed XYZ channels')
        normal = normal[..., :3]
        length = np.linalg.norm(normal, axis=2)
        mask &= np.isfinite(normal).all(axis=2) & (length > 1e-8)
        normal = normal / np.maximum(length[..., None], 1e-8)
        for adjacent in neighbors(normal):
            mask &= np.sum(normal * adjacent, axis=2) >= args.normal_dot
    albedo = guides.get('albedo')
    if albedo is not None:
        if albedo.shape[2] < 3:
            raise ValueError('Albedo guide requires RGB channels')
        albedo = albedo[..., :3]
        mask &= np.isfinite(albedo).all(axis=2)
        for adjacent in neighbors(albedo):
            mask &= np.linalg.norm(albedo - adjacent, axis=2) <= args.albedo_edge
    identity = guides.get('surface')
    if identity is not None:
        identity = identity[..., 0]
        mask &= np.isfinite(identity) & (identity != 0xffffffff)
        if args.surface_id:
            mask &= np.isin(identity, args.surface_id)
        for adjacent in neighbors(identity):
            mask &= adjacent == identity
    if 'mask' in guides:
        mask &= np.isfinite(guides['mask'][..., 0]) & (guides['mask'][..., 0] > 0)
    if any(key in guides for key in ('depth', 'normal', 'albedo', 'surface')):
        mask[[0, -1], :] = False
        mask[:, [0, -1]] = False
    for _ in range(args.erode):
        previous = mask.copy()
        for adjacent in neighbors(previous):
            mask &= adjacent
        mask[[0, -1], :] = False
        mask[:, [0, -1]] = False
    return mask


def rectangle_factor(distance, height, half_width):
    if min(distance, height, half_width) <= 0:
        raise ValueError('Rectangle dimensions must be positive')
    diagonal = math.hypot(distance, height)
    return (math.atan(half_width / distance) - distance / diagonal *
            math.atan(half_width / diagonal)) / math.pi


def self_test():
    # Floor n=+Y, receiver=(d,0,0), source x=0, y=[0,H], z=[-W,W].
    # Cosine PDF cancels Lambert cos/pi; constant source contribution is a
    # Bernoulli variable with mean equal to this diffuse configuration factor.
    distance, height, half_width = 1., 2., 2.
    exact = rectangle_factor(distance, height, half_width)
    rng = np.random.default_rng(61731)
    count = 400000
    u, v = rng.random((2, count))
    radius, phi = np.sqrt(u), 2 * np.pi * v
    x, y, z = radius * np.cos(phi), np.sqrt(1 - u), radius * np.sin(phi)
    t = np.divide(-distance, x, out=np.zeros_like(x), where=x < 0)
    hits = (x < 0) & (t * y < height) & (np.abs(t * z) < half_width)
    estimate = float(hits.mean())
    standard_error = math.sqrt(exact * (1 - exact) / count)
    assert abs(estimate - exact) < 5 * standard_error
    # Independent deterministic area integration; not the screen-space tracer.
    cells = 800
    yy = (np.arange(cells) + .5) * height / cells
    zz = (np.arange(cells) + .5) * 2 * half_width / cells - half_width
    kernel = distance * yy[:, None] / (distance**2 + yy[:, None]**2 + zz[None, :]**2)**2
    quadrature = float(kernel.sum() * height * 2 * half_width / cells**2 / np.pi)
    assert abs(quadrature - exact) < 2e-6
    # A minimal two-tap model of center-only A-trous luminance rejection.
    signal = np.array([0., 100.])
    sigma = np.array([1e-4, 100.])
    weights = np.exp(-np.abs(signal[:, None] - signal[None, :]) / sigma[:, None])
    filtered = (weights @ signal) / weights.sum(axis=1)
    assert filtered[0] == 0 and filtered.sum() < signal.sum()
    with tempfile.TemporaryDirectory() as directory:
        pfm = Path(directory) / 'fixture.pfm'
        expected = np.arange(18, dtype=np.float32).reshape(2, 3, 3)
        pfm.write_bytes(b'PF\n3 2\n-1.0\n' + expected[::-1].astype('<f4').tobytes())
        assert np.array_equal(load_image(pfm)[0], expected)
        import gzip
        archived = pfm.with_suffix('.pfm.gz')
        archived.write_bytes(gzip.compress(pfm.read_bytes()))
        pfm.unlink()
        assert np.array_equal(load_image(pfm)[0], expected)
        assert np.array_equal(load_image(archived)[0], expected)
        raw = Path(directory) / 'fixture.raw'
        special = np.array([0, -1, 65504, np.nan, np.inf, -np.inf], dtype='<f2').reshape(2, 3, 1)
        raw.write_bytes(special.tobytes())
        Path(str(raw) + '.json').write_text(json.dumps({
            'schema': 'hengine.texture-readback.raw.v1', 'height': 2, 'width': 3,
            'channels': 1, 'dtype': 'float16', 'endianness': 'little',
            'layout': 'HWC', 'row_order': 'top-down'}))
        loaded = load_image(raw)[0]
        assert np.array_equal(loaded, special, equal_nan=True)
        stats = summary(loaded)
        assert stats['nan'] == stats['positive_inf'] == stats['negative_inf'] == stats['at_fp16_max'] == 1
        # End-to-end fixed-ROI sequence checks: means 2 and 4, population
        # temporal variance 1. A zero-albedo pixel remains eligible (no boost).
        captures = []
        for frame_index, value in enumerate((2., 4.)):
            capture = Path(directory) / f'frame-{frame_index}.pfm'
            capture.write_bytes(b'PF\n8 8\n-1.0\n' + np.full((8, 8, 3), value, dtype='<f4').tobytes())
            captures.append(str(capture))
        guide = Path(directory) / 'depth.pfm'
        guide.write_bytes(b'Pf\n8 8\n-1.0\n' + np.full((8, 8, 1), .5, dtype='<f4').tobytes())
        settings = SimpleNamespace(input=captures, roi=['test:2,2,6,6'], normalized_roi=False,
                                   depth=[str(guide)], normal=None, albedo=None, surface=None, mask=None,
                                   surface_id=[], depth_kind='device', depth_edge=.0015,
                                   normal_dot=.9, albedo_edge=.25, erode=1, units='synthetic scalar radiance', scale=1., channels=None)
        analysis = analyze(settings)
        assert analysis['temporal']['test']['common_pixels'] == 16
        assert abs(analysis['temporal']['test']['population_variance']['mean'] - 1.) < 1e-12
        assert analysis['frames'][0]['regions']['test']['selected_pixels'] == 16
        settings.input = captures[:1]
        settings.mask = None
        settings.roi = []
        try:
            analyze(settings)
        except ValueError:
            pass
        else:
            raise AssertionError('Unspecified full-frame analysis must be rejected')
    return {'evidence': 'unit-test', 'status': 'PASS', 'gpu_executed': False,
            'rectangle': {'distance': distance, 'height': height, 'half_width': half_width,
                          'analytic_factor': exact, 'quadrature_factor': quadrature,
                          'cosine_mc_factor': estimate, 'samples': count,
                          'standard_error': standard_error, 'tolerance': '5 standard errors',
                          'outgoing_radiance': 'diffuse_reflectance * source_radiance * factor'},
            'sparse_filter_model': {'input': signal.tolist(), 'output': filtered.tolist(),
                                    'energy_ratio': float(filtered.sum() / signal.sum()),
                                    'scope': 'Two-tap center-variance counterexample, not full SVGF or a GPU quality test'},
            'file_decoders': 'PFM orientation and raw float16 non-finite preservation PASS',
            'roi_sequence': 'Explicit masked ROI counts and population temporal variance PASS'}


def expand(paths):
    result = []
    for item in paths or []:
        matches = sorted(glob.glob(item)) if glob.has_magic(item) else [item]
        if not matches:
            raise ValueError(f'No files matched: {item}')
        result.extend(Path(match) for match in matches)
    return result


def analyze(args):
    paths = expand(args.input)
    if not paths:
        raise ValueError('No input captures matched')
    if not args.roi and not args.mask and not args.surface_id:
        raise ValueError('Provide explicit --roi, --mask or --surface-id; no default all-black full-frame acceptance')
    if args.surface_id and not args.surface:
        raise ValueError('--surface-id requires --surface')
    names = [spec.split(':', 1)[0] for spec in args.roi]
    if len(names) != len(set(names)):
        raise ValueError('ROI names must be unique')
    guides = {key: expand(getattr(args, key)) for key in ('depth', 'normal', 'albedo', 'surface', 'mask')}
    for key, files in guides.items():
        if len(files) not in (0, 1, len(paths)):
            raise ValueError(f'{key}: expected one static guide or {len(paths)} per-frame guides')
    report = {'evidence': 'capture-analysis', 'acceptance': 'NOT EVALUATED',
              'units': args.units, 'scale_applied': args.scale,
              'limitations': ['Statistics cannot establish source buffer semantics or physical correctness.',
                             'fp16 maximum counts indicate potential saturation, not proof of clipping.',
                             'Temporal variance uses common fixed pixels without motion compensation.'],
              'controls': {key: value for key, value in vars(args).items() if key != 'output'},
              'frames': [], 'temporal': {}}
    states = {}
    first_shape = None
    for index, path in enumerate(paths):
        if path.suffix.lower() == '.png':
            raise ValueError('PNG is a mask-only input; HDR analysis requires PFM/raw')
        image, metadata = load_image(path)
        if args.channels:
            channels = [int(channel) for channel in args.channels.split(',')]
            if not channels or min(channels) < 0 or max(channels) >= image.shape[2]:
                raise ValueError('Selected channel is outside the native capture')
            native = image[..., channels]
        else:
            if path.suffix.lower() == '.raw':
                raise ValueError('Native raw requires --channels (depth 0; hit coverage 3; radiance 0,1,2)')
            native = image[..., :3]
        image = native * args.scale
        shape = image.shape[:2]
        if first_shape is not None and first_shape != image.shape:
            raise ValueError('Sequence dimensions/channels changed')
        first_shape = image.shape
        frame_guides = {}
        for key, files in guides.items():
            if files:
                guide, _ = load_image(files[0 if len(files) == 1 else index])
                if guide.shape[:2] != shape:
                    raise ValueError(f'{key}: guide resolution must match capture; no implicit resampling')
                frame_guides[key] = guide
        mask = surface_mask(shape, frame_guides, args)
        frame = {'path': str(path), 'metadata': metadata, 'width': shape[1], 'height': shape[0], 'regions': {}}
        regions = args.roi or ['surface:0,0,1,1' if args.normalized_roi else f'surface:0,0,{shape[1]},{shape[0]}']
        for spec in regions:
            name, bounds = spec.split(':', 1)
            coords = list(map(float, bounds.split(',')))
            if len(coords) != 4:
                raise ValueError('ROI format: name:x0,y0,x1,y1')
            if args.normalized_roi:
                coords = [v * shape[1 if axis % 2 == 0 else 0] for axis, v in enumerate(coords)]
            x0, y0, x1, y1 = map(lambda value: int(round(value)), coords)
            if not (0 <= x0 < x1 <= shape[1] and 0 <= y0 < y1 <= shape[0]):
                raise ValueError(f'ROI outside capture: {spec}')
            section = image[y0:y1, x0:x1]
            selected = mask[y0:y1, x0:x1]
            pixels = section[selected]
            with np.errstate(invalid='ignore'):
                luminance = section @ LUMA if image.shape[2] == 3 else section[..., 0]
            frame['regions'][name] = {'bounds': [x0, y0, x1, y1], 'selected_pixels': int(selected.sum()),
                                      'excluded_pixels': int(selected.size - selected.sum()),
                                      'native_at_fp16_max_components': int((native[y0:y1, x0:x1][selected] == 65504).sum()),
                                      'validity': 'NONEMPTY' if selected.any() else 'EMPTY ROI: no energy evidence',
                                      'channels': [summary(pixels[:, c]) for c in range(image.shape[2])],
                                      'luminance': summary(luminance[selected])}
            valid = selected & np.isfinite(luminance)
            if name not in states:
                states[name] = {'common': valid.copy(), 'mean': np.zeros_like(luminance),
                                'm2': np.zeros_like(luminance), 'count': np.zeros_like(luminance),
                                'roi_means': []}
            state = states[name]
            state['common'] &= valid
            state['count'][valid] += 1
            delta = luminance[valid] - state['mean'][valid]
            state['mean'][valid] += delta / state['count'][valid]
            state['m2'][valid] += delta * (luminance[valid] - state['mean'][valid])
            state['roi_means'].append(float(luminance[valid].mean()) if valid.any() else None)
        report['frames'].append(frame)
    for name, state in states.items():
        common = state['common']
        report['temporal'][name] = {'frames': len(paths), 'common_pixels': int(common.sum()),
                                    'per_frame_roi_luminance_mean': state['roi_means'],
                                    'population_variance': summary(state['m2'][common] / len(paths))}
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--input', nargs='+')
    parser.add_argument('--roi', action='append', default=[])
    parser.add_argument('--normalized-roi', action='store_true')
    for key in ('depth', 'normal', 'albedo', 'surface', 'mask'):
        parser.add_argument('--' + key, nargs='+')
    parser.add_argument('--surface-id', type=int, action='append', default=[])
    parser.add_argument('--depth-kind', choices=['device', 'linear'], default='device')
    parser.add_argument('--depth-edge', type=float, default=.0015)
    parser.add_argument('--normal-dot', type=float, default=.9)
    parser.add_argument('--albedo-edge', type=float, default=.25)
    parser.add_argument('--erode', type=int, default=1)
    parser.add_argument('--units', default='unspecified: caller must identify radiance/illumination/data and exposure domain')
    parser.add_argument('--channels', help='Explicit native channel indices, e.g. 0 or 0,1,2 or 3 for SSGI hit coverage')
    parser.add_argument('--scale', type=float, default=1., help='Explicit unit/exposure conversion only, recorded in output')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--rectangle', type=float, nargs=3, metavar=('D', 'H', 'W'),
                        help='Analytic floor receiver distance, source height and half-width')
    parser.add_argument('--source-radiance', type=float, default=1.)
    parser.add_argument('--diffuse-reflectance', type=float, default=1.)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if (args.erode < 0 or args.depth_edge < 0 or args.albedo_edge < 0 or
            not -1 <= args.normal_dot <= 1 or not math.isfinite(args.scale) or args.scale <= 0):
        parser.error('Invalid mask thresholds/scale')
    if not args.self_test and not args.input and not args.rectangle:
        parser.error('Provide --input, --rectangle or --self-test')
    report = {}
    if args.self_test:
        report['self_test'] = self_test()
    if args.rectangle:
        if (not all(math.isfinite(v) and v > 0 for v in args.rectangle) or
                not math.isfinite(args.source_radiance) or args.source_radiance < 0 or
                not 0 <= args.diffuse_reflectance <= 1):
            parser.error('Invalid rectangle/radiance/reflectance')
        factor = rectangle_factor(*args.rectangle)
        report['analytic_rectangle'] = {
            'gpu_executed': False, 'dimensions_d_h_w': args.rectangle,
            'geometry': 'Receiver (d,0,0), normal +Y; uniform source x=0, y=[0,H], z=[-W,W], normal +X; no blockers',
            'factor': factor, 'source_radiance': args.source_radiance,
            'diffuse_reflectance': args.diffuse_reflectance,
            'outgoing_radiance': factor * args.source_radiance * args.diffuse_reflectance}
    if args.input:
        report['analysis'] = analyze(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps({'output': str(args.output), 'self_test': report.get('self_test', {}).get('status'),
                      'analyzed_frames': len(report.get('analysis', {}).get('frames', []))}))


if __name__ == '__main__':
    main()
