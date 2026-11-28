#!/usr/bin/env python3
"""frame_analysis.py - R22 menus-push capture/readback evidence reader.

Reads one Palmetto boot evidence directory (a capture dir with index.txt, a
readback PPM dir, and the meta/summary text beside them), reports per-frame RGB
mean/min/max, flags non-black frames, writes the best frame as a PNG, and
prints a one-command summary for any arm evidence directory.

Standard library only.  It reads the PPM/text evidence a boot wrote outside
the repository and writes its analysis beside it; it never opens the game
package, so no game bytes are copied or hashed.

Usage:
  python -I tooling/scripts/frame_analysis.py arm <boot-dir|tag> --root <fsrstable> [--out DIR]
  python -I tooling/scripts/frame_analysis.py capture <capture-dir>
  python -I tooling/scripts/frame_analysis.py readback <readback-dir>
  python -I tooling/scripts/frame_analysis.py images <dir-of-png-ppm>
  python -I tooling/scripts/frame_analysis.py self-test

`tests/test_frame_analysis.py` builds synthetic PPMs and checks this reader;
the self-test above does the same from the command line.

Exit codes: 0 ok; 1 usage/IO error; 2 --fail-if-black fired;
            3 --fail-if-nonblack fired.
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

SWITCHES = [
    'PALMETTO_CAPTURE', 'PALMETTO_CAPTURE_EVERY', 'PALMETTO_CAPTURE_LIMIT',
    'PALMETTO_READBACK_PPM', 'PALMETTO_READBACK_PPM_LIMIT',
    'PALMETTO_READBACK_AT_DRAW', 'PALMETTO_READBACK_FULL',
    'PALMETTO_READBACK_CENTRE', 'PALMETTO_PRESENT_SYNTHETIC',
    'PALMETTO_PRESENT_FIT', 'PALMETTO_PRESENT_GEOMETRY',
    'PALMETTO_PRESENT_ATTEST', 'PALMETTO_TITLE_CLASS0A_SIZE',
    'PALMETTO_CONTENT_PROBE', 'PALMETTO_SLOT70_PUBLISH',
    'PALMETTO_STATE_TRACE', 'PALMETTO_UI_PROBE',
]
WRITE_CONTROL = (51, 102, 204)
SYNTHETIC_BANDS = ((204, 51, 102), (51, 204, 102), (51, 102, 204), (255, 255, 255))
BAND_TOL = 3
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def read_ppm(path):
    with open(path, 'rb') as handle:
        raw = handle.read()
    if raw[:2] != b'P6':
        return {'error': 'not a binary PPM (magic %r)' % raw[:2]}
    pos = 2
    tokens = []
    while len(tokens) < 3:
        while pos < len(raw) and raw[pos] in b' \t\r\n':
            pos += 1
        if pos >= len(raw):
            return {'error': 'truncated PPM header'}
        start = pos
        while pos < len(raw) and raw[pos] not in b' \t\r\n':
            pos += 1
        tokens.append(raw[start:pos])
    try:
        width = int(tokens[0])
        height = int(tokens[1])
        maxval = int(tokens[2])
    except ValueError:
        return {'error': 'non-numeric PPM header %r' % (tokens,)}
    if maxval != 255:
        return {'error': 'unsupported maxval %d (only 255)' % maxval}
    if width <= 0 or height <= 0:
        return {'error': 'bad PPM extent %dx%d' % (width, height)}
    if raw[pos:pos + 2] == b'\r\n':
        pos += 2
    elif pos < len(raw):
        pos += 1
    pixels = width * height
    needed = pixels * 3
    available = len(raw) - pos
    if available < needed:
        return {'error': 'truncated pixel data: %d of %d bytes' % (available, needed),
                'width': width, 'height': height}
    return {'width': width, 'height': height, 'data': raw[pos:pos + needed],
            'trailing': available - needed, 'header_bytes': pos,
            'file_bytes': len(raw)}


def pixel_stats(data, width, height):
    pixels = width * height
    planes = (data[0::3], data[1::3], data[2::3])
    sums = [sum(plane) for plane in planes]
    mins = [min(plane) for plane in planes]
    maxs = [max(plane) for plane in planes]
    means = [sums[i] / float(pixels) for i in range(3)]
    black = maxs == [0, 0, 0]
    uniform = mins == maxs
    if black:
        non_black = 0
    else:
        value = int.from_bytes(planes[0], 'big')
        value |= int.from_bytes(planes[1], 'big')
        value |= int.from_bytes(planes[2], 'big')
        non_black = pixels - value.to_bytes(pixels, 'big').count(bytes(1))
    return {'mean': means, 'min': mins, 'max': maxs, 'sum': sums,
            'pixels': pixels, 'non_black': non_black, 'peak': max(maxs),
            'black': black, 'uniform': uniform,
            'colour': list(maxs) if uniform else None}


def band_probe(data, width, height):
    row = height // 2
    probes = []
    for index in range(4):
        column = (width * (2 * index + 1)) // 8
        offset = (row * width + column) * 3
        probes.append(list(data[offset:offset + 3]))
    return probes


def synthetic_match(probes):
    if len(probes) != 4:
        return False
    for got, want in zip(probes, SYNTHETIC_BANDS):
        if len(got) != 3:
            return False
        if any(abs(got[i] - want[i]) > BAND_TOL for i in range(3)):
            return False
    return True


def _png_chunk(tag, payload):
    return (struct.pack('>I', len(payload)) + tag + payload
            + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF))


def write_png(path, width, height, data, max_dim=0):
    if max_dim and (width > max_dim or height > max_dim):
        step = (max(width, height) + max_dim - 1) // max_dim
        out_w = (width + step - 1) // step
        out_h = (height + step - 1) // step
        rows = []
        for oy in range(out_h):
            sy = min(height - 1, oy * step)
            src = data[sy * width * 3:(sy + 1) * width * 3]
            row = bytearray(out_w * 3)
            for ox in range(out_w):
                sx = min(width - 1, ox * step)
                row[ox * 3:ox * 3 + 3] = src[sx * 3:sx * 3 + 3]
            rows.append(bytes(1) + bytes(row))
        width, height = out_w, out_h
    else:
        rows = [bytes(1) + data[y * width * 3:(y + 1) * width * 3]
                for y in range(height)]
    raw = b''.join(rows)
    payload = (PNG_SIGNATURE
               + _png_chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
               + _png_chunk(b'IDAT', zlib.compress(raw, 6))
               + _png_chunk(b'IEND', b''))
    with open(path, 'wb') as handle:
        handle.write(payload)
    return {'path': path, 'width': width, 'height': height, 'bytes': len(payload)}



def _png_paeth(left, up, upleft):
    prediction = left + up - upleft
    distance_left = abs(prediction - left)
    distance_up = abs(prediction - up)
    distance_upleft = abs(prediction - upleft)
    if distance_left <= distance_up and distance_left <= distance_upleft:
        return left
    if distance_up <= distance_upleft:
        return up
    return upleft


def read_png(path):
    """Decode an 8-bit non-interlaced PNG to RGB bytes (standard library only).

    A black PNG is recognised from its zero row residuals without unfiltering, so
    a large all-black window screenshot costs one decompress and no per-byte loop.
    Any other PNG is unfiltered with the PNG predictors (None/Sub/Up/Average/Paeth).
    """
    with open(path, 'rb') as handle:
        raw = handle.read()
    if raw[:8] != PNG_SIGNATURE:
        return {'error': 'not a PNG (bad signature)'}
    position = 8
    idat = []
    width = None
    height = None
    bit_depth = None
    colour_type = None
    interlace = None
    while position + 8 <= len(raw):
        length = struct.unpack('>I', raw[position:position + 4])[0]
        tag = raw[position + 4:position + 8]
        payload = raw[position + 8:position + 8 + length]
        if tag == b'IHDR':
            if len(payload) < 13:
                return {'error': 'short IHDR chunk'}
            width, height = struct.unpack('>II', payload[:8])
            bit_depth = payload[8]
            colour_type = payload[9]
            interlace = payload[12]
        elif tag == b'IDAT':
            idat.append(payload)
        elif tag == b'IEND':
            break
        position += 12 + length
    if width is None or height is None:
        return {'error': 'no IHDR chunk'}
    if bit_depth != 8:
        return {'error': 'unsupported PNG bit depth %d (only 8)' % bit_depth}
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(colour_type)
    if channels is None:
        return {'error': 'unsupported PNG colour type %d' % colour_type}
    if interlace != 0:
        return {'error': 'interlaced PNG is not supported'}
    if width <= 0 or height <= 0:
        return {'error': 'bad PNG extent %dx%d' % (width, height)}
    if width * height > 60000000:
        return {'error': 'PNG too large to decode in pure Python (%dx%d)' % (width, height)}
    try:
        stream = zlib.decompress(b''.join(idat))
    except zlib.error as error:
        return {'error': 'bad PNG IDAT stream: %s' % error}
    stride = width * channels
    if len(stream) < height * (stride + 1):
        return {'error': 'short PNG image data'}
    residual_zero = True
    offset = 0
    for _y in range(height):
        if stream[offset + 1:offset + 1 + stride].strip(bytes(1)):
            residual_zero = False
            break
        offset += stride + 1
    if residual_zero:
        return {'width': width, 'height': height,
                'data': bytes(width * height * 3), 'channels': channels,
                'file_bytes': len(raw), 'filters': 'zero residuals (black fast path)'}
    data = bytearray(width * height * 3)
    previous = bytearray(stride)
    offset = 0
    filters = {}
    for y in range(height):
        filter_type = stream[offset]
        offset += 1
        row = bytearray(stream[offset:offset + stride])
        offset += stride
        filters[filter_type] = filters.get(filter_type, 0) + 1
        if filter_type == 0:
            pass
        elif filter_type == 1:
            for i in range(channels, stride):
                row[i] = (row[i] + row[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                row[i] = (row[i] + previous[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                row[i] = (row[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                up = previous[i]
                upleft = previous[i - channels] if i >= channels else 0
                row[i] = (row[i] + _png_paeth(left, up, upleft)) & 0xFF
        else:
            return {'error': 'unknown PNG filter type %d' % filter_type}
        target = y * width * 3
        if channels == 3:
            data[target:target + width * 3] = row
        else:
            for x in range(width):
                source = x * channels
                data[target + x * 3] = row[source]
                data[target + x * 3 + 1] = row[source + 1]
                data[target + x * 3 + 2] = row[source + 2]
        previous = row
    return {'width': width, 'height': height, 'data': bytes(data),
            'channels': channels, 'file_bytes': len(raw), 'filters': filters}


def read_image(path):
    if path.lower().endswith('.png'):
        return read_png(path)
    return read_ppm(path)

def parse_capture_index(path):
    entries = []
    malformed = []
    with open(path, 'r', encoding='utf-8', errors='replace') as handle:
        for line_number, line in enumerate(handle, 1):
            text = line.strip()
            if not text or text.startswith('#'):
                continue
            tokens = text.split()
            try:
                number = int(tokens[tokens.index('frame') + 1])
                fmt = int(tokens[tokens.index('format') + 1])
                extent = tokens[tokens.index('extent') + 1]
                width_text, height_text = extent.split('x')
                mean_at = tokens.index('mean')
                index_mean = [float(tokens[mean_at + 2]),
                              float(tokens[mean_at + 3]),
                              float(tokens[mean_at + 4])]
            except (ValueError, IndexError):
                malformed.append({'line': line_number, 'text': text})
                continue
            entries.append({'frame': number, 'format': fmt,
                            'width': int(width_text), 'height': int(height_text),
                            'index_mean': index_mean, 'line': line_number})
    return entries, malformed


def frame_number(name):
    stem = os.path.splitext(name)[0]
    if not stem.startswith('frame_'):
        return None
    try:
        return int(stem.split('_', 1)[1])
    except ValueError:
        return None


def parse_readback_name(name):
    stem = os.path.splitext(name)[0]
    if not stem.startswith('readback_draw'):
        return None
    parts = stem[len('readback_draw'):].split('_')
    if len(parts) < 3:
        return None
    try:
        draw = int(parts[0])
    except ValueError:
        return None
    size = parts[-1]
    try:
        width_text, height_text = size.split('x')
        width, height = int(width_text), int(height_text)
    except ValueError:
        return None
    resource = parts[1][3:] if parts[1].startswith('res') else parts[1]
    return {'draw': draw, 'resource': resource, 'width': width, 'height': height}


def analyse_frame_file(path):
    ppm = read_image(path)
    if 'error' in ppm:
        return {'file': os.path.basename(path), 'error': ppm['error'],
                'width': ppm.get('width'), 'height': ppm.get('height')}
    stats = pixel_stats(ppm['data'], ppm['width'], ppm['height'])
    probes = band_probe(ppm['data'], ppm['width'], ppm['height'])
    stats.update({'file': os.path.basename(path),
                  'path': path,
                  'width': ppm['width'], 'height': ppm['height'],
                  'trailing': ppm.get('trailing', 0),
                  'header_bytes': ppm.get('header_bytes', 0),
                  'file_bytes': ppm['file_bytes'],
                  'band_probe': probes,
                  'synthetic': synthetic_match(probes)})
    return stats


def analyse_capture(directory):
    if not os.path.isdir(directory):
        return {'error': 'capture directory not found: %s' % directory}
    report = {'mode': 'capture', 'directory': os.path.abspath(directory),
              'frames': [], 'errors': [], 'checks': []}
    names = sorted(name for name in os.listdir(directory)
                   if name.startswith('frame_') and name.lower().endswith('.ppm'))
    for name in names:
        frame = analyse_frame_file(os.path.join(directory, name))
        frame['number'] = frame_number(name)
        report['frames'].append(frame)
    index_path = os.path.join(directory, 'index.txt')
    if os.path.isfile(index_path):
        entries, malformed = parse_capture_index(index_path)
        by_number = {entry['frame']: entry for entry in entries}
        file_numbers = {frame['number'] for frame in report['frames']
                        if frame.get('number') is not None}
        missing = sorted(set(by_number) - file_numbers)
        extra = sorted(number for number in file_numbers if number not in by_number)
        report['checks'].append({'check': 'index_vs_files',
                                 'index_entries': len(entries),
                                 'frame_files': len(names),
                                 'index_without_file': missing,
                                 'file_without_index': extra,
                                 'malformed': malformed,
                                 'ok': not missing and not extra and not malformed})
        mismatched = []
        for frame in report['frames']:
            entry = by_number.get(frame.get('number'))
            if not entry or 'error' in frame:
                continue
            problems = []
            if (entry['width'], entry['height']) != (frame['width'], frame['height']):
                problems.append('extent %dx%d vs PPM header %dx%d'
                                % (entry['width'], entry['height'],
                                   frame['width'], frame['height']))
            for channel in range(3):
                if abs(frame['mean'][channel] - entry['index_mean'][channel]) > 0.02:
                    problems.append('mean %s vs index %s'
                                    % ('%.2f %.2f %.2f' % tuple(frame['mean']),
                                       '%.2f %.2f %.2f' % tuple(entry['index_mean'])))
                    break
            if problems:
                mismatched.append({'frame': entry['frame'], 'problems': problems})
        report['checks'].append({'check': 'index_vs_computed', 'tolerance': 0.02,
                                 'mismatched': mismatched,
                                 'ok': not mismatched})
        report['index'] = {'path': index_path, 'entries': len(entries),
                           'malformed': malformed,
                           'sha256': sha256_file(index_path)}
    else:
        report['index'] = None
        report['checks'].append({'check': 'index_vs_files', 'ok': False,
                                 'detail': 'no index.txt in the capture directory'})
    for frame in report['frames']:
        if 'error' in frame:
            report['errors'].append({'file': frame['file'], 'error': frame['error']})
    usable = [frame for frame in report['frames'] if 'error' not in frame]
    black = [frame for frame in usable if frame['black']]
    non_black = [frame for frame in usable if not frame['black']]
    summary = {'frame_files': len(names), 'usable': len(usable),
               'black': len(black), 'non_black': len(non_black),
               'synthetic_matches': len([f for f in usable if f['synthetic']]),
               'extents': sorted(set('%dx%d' % (f['width'], f['height']) for f in usable)),
               'any_non_black': bool(non_black),
               'best_frame': None, 'best_frame_path': None,
               'best_mean': None}
    if usable:
        best = max(usable, key=lambda f: (0 if f['synthetic'] else 1,
                                          f['non_black'], sum(f['mean'])))
        summary['best_frame'] = best['file']
        summary['best_frame_path'] = best['path']
        summary['best_mean'] = best['mean']
    report['summary'] = summary
    return report


def classify_readback(frame):
    if 'error' in frame:
        return 'error'
    if frame['black']:
        return 'black'
    if frame['uniform'] and tuple(frame['colour']) == WRITE_CONTROL:
        return 'write_control'
    if frame['uniform']:
        return 'uniform'
    if frame['synthetic']:
        return 'synthetic_four_band'
    return 'content'


def analyse_readback(directory):
    if not os.path.isdir(directory):
        return {'error': 'readback directory not found: %s' % directory}
    report = {'mode': 'readback', 'directory': os.path.abspath(directory),
              'frames': [], 'errors': [], 'checks': []}
    names = sorted(name for name in os.listdir(directory)
                   if name.startswith('readback_') and name.lower().endswith('.ppm'))
    for name in names:
        frame = analyse_frame_file(os.path.join(directory, name))
        frame['readback'] = parse_readback_name(name)
        frame['class'] = classify_readback(frame)
        report['frames'].append(frame)
    for frame in report['frames']:
        if 'error' in frame:
            report['errors'].append({'file': frame['file'], 'error': frame['error']})
    usable = [frame for frame in report['frames'] if 'error' not in frame]
    counts = {}
    for frame in usable:
        counts[frame['class']] = counts.get(frame['class'], 0) + 1
    content = [frame for frame in usable if frame['class'] == 'content']
    summary = {'files': len(names), 'usable': len(usable),
               'black': counts.get('black', 0),
               'write_control': counts.get('write_control', 0),
               'uniform': counts.get('uniform', 0),
               'synthetic_four_band': counts.get('synthetic_four_band', 0),
               'content': counts.get('content', 0),
               'extents': sorted(set('%dx%d' % (f['width'], f['height']) for f in usable)),
               'any_content': bool(content),
               'best_frame': None, 'best_frame_path': None}
    if usable:
        best = max(usable, key=lambda f: (f['class'] == 'content',
                                          f['non_black'], sum(f['mean'])))
        summary['best_frame'] = best['file']
        summary['best_frame_path'] = best['path']
    report['summary'] = summary
    return report


def read_text_lines(path):
    if not os.path.isfile(path):
        return []
    with open(path, 'r', encoding='utf-8', errors='replace') as handle:
        return handle.readlines()


def parse_meta(path):
    if not os.path.isfile(path):
        return None
    meta = {'path': path, 'values': {}, 'env': {}, 'env_duplicates': {}}
    for line in read_text_lines(path):
        if '=' not in line:
            continue
        key, value = line.split('=', 1)
        key = key.strip()
        value = value.rstrip('\r\n')
        if key == 'env':
            for token in value.split():
                if '=' not in token:
                    continue
                name, val = token.split('=', 1)
                if name in meta['env']:
                    meta['env_duplicates'][name] = meta['env_duplicates'].get(name, 0) + 1
                meta['env'][name] = val
            continue
        meta['values'][key] = value
    return meta


def parse_summary(boot):
    info = {'source': None, 'verdict': None, 'wall': None, 'exceptions': None,
            'd3d12': None, 'exit_code': None, 'timeout': None, 'trace': None}
    prefixes = (('verdict', 'VERDICT:'), ('wall', 'wall time:'),
                ('exceptions', 'first exception:'), ('d3d12', 'd3d12:'),
                ('exit_code', 'exit code:'), ('timeout', 'timeout:'),
                ('trace', 'trace:'))
    for name in ('summary_console.txt', 'summary.txt'):
        path = os.path.join(boot, name)
        if not os.path.isfile(path):
            continue
        info['source'] = path
        for line in read_text_lines(path):
            text = line.strip()
            for key, prefix in prefixes:
                if info[key] is None and text.startswith(prefix):
                    info[key] = text[len(prefix):].strip()
        break
    return info


def scan_stderr(boot):
    path = os.path.join(boot, 'stderr.log')
    counts = {'device_lost': 0, 'present_failed': 0, 'capture_lines': 0,
              'readback_lines': 0, 'cannot_write': 0,
              'present_attest_lines': 0, 'last_present_attest': None,
              'device_lost_first': None,
              'present_failed_first': None, 'bytes': 0}
    if not os.path.isfile(path):
        counts['missing'] = True
        return counts
    counts['bytes'] = os.path.getsize(path)
    with open(path, 'r', encoding='utf-8', errors='replace') as handle:
        for line in handle:
            if 'the Vulkan device was lost' in line:
                counts['device_lost'] += 1
                if counts['device_lost_first'] is None:
                    counts['device_lost_first'] = line.strip()[:160]
            elif 'present failed (VkResult' in line:
                counts['present_failed'] += 1
                if counts['present_failed_first'] is None:
                    counts['present_failed_first'] = line.strip()[:160]
            elif 'present: capture frame_' in line:
                counts['capture_lines'] += 1
            elif 'present: cannot write' in line:
                counts['cannot_write'] += 1
            elif 'present attest' in line and 'presents' in line:
                counts['present_attest_lines'] += 1
                counts['last_present_attest'] = line.strip()[:220]
            elif 'replay: readback ppm ' in line:
                counts['readback_lines'] += 1
    return counts


def looks_like_capture(directory):
    if not os.path.isdir(directory):
        return False
    if os.path.isfile(os.path.join(directory, 'index.txt')):
        return True
    try:
        names = os.listdir(directory)
    except OSError:
        return False
    return any(name.startswith('frame_') and name.lower().endswith('.ppm')
               for name in names)


def looks_like_readback(directory):
    if not os.path.isdir(directory):
        return False
    try:
        names = os.listdir(directory)
    except OSError:
        return False
    return any(name.startswith('readback_') and name.lower().endswith('.ppm')
               for name in names)


def _append_unique(items, value):
    if value not in items:
        items.append(value)


def resolve_arm(argument, root):
    boot = None
    if argument and os.path.isdir(argument):
        boot = os.path.abspath(argument)
    elif argument:
        candidates = []
        if root:
            candidates.append(os.path.join(root, 'boot-' + argument))
            candidates.append(os.path.join(root, argument))
        candidates.append(os.path.abspath(argument))
        for candidate in candidates:
            if os.path.isdir(candidate):
                boot = candidate
                break
    if boot is None:
        return None
    name = os.path.basename(boot)
    tag = name[5:] if name.startswith('boot-') else name
    parent = os.path.dirname(boot)
    meta = parse_meta(os.path.join(boot, 'meta.txt'))
    captures = []
    readbacks = []
    empty_captures = []
    empty_readbacks = []
    missing_captures = []
    missing_readbacks = []

    def note_capture(candidate):
        if looks_like_capture(candidate):
            _append_unique(captures, os.path.abspath(candidate))
        elif os.path.isdir(candidate):
            _append_unique(empty_captures, os.path.abspath(candidate))

    def note_readback(candidate):
        if looks_like_readback(candidate):
            _append_unique(readbacks, os.path.abspath(candidate))
        elif os.path.isdir(candidate):
            _append_unique(empty_readbacks, os.path.abspath(candidate))

    for candidate in (os.path.join(parent, 'capture-' + tag),
                      os.path.join(boot, 'capture'),
                      os.path.join(boot, 'capture-' + tag)):
        note_capture(candidate)
    for candidate in (os.path.join(parent, 'readback-' + tag),
                      os.path.join(boot, 'readback'),
                      os.path.join(boot, 'readback-' + tag)):
        note_readback(candidate)
    env = meta['env'] if meta else {}
    suppressed = {}
    if env.get('PALMETTO_CAPTURE') == '':
        suppressed['capture'] = 'PALMETTO_CAPTURE was overridden to an empty value (captures suppressed by design)'
    if env.get('PALMETTO_READBACK_PPM') == '':
        suppressed['readback'] = 'PALMETTO_READBACK_PPM was overridden to an empty value (readbacks suppressed by design)'
    for switch, kind in (('PALMETTO_CAPTURE', 'capture'),
                         ('PALMETTO_READBACK_PPM', 'readback')):
        value = env.get(switch)
        if not value:
            continue
        if os.path.isdir(value):
            if kind == 'capture':
                note_capture(value)
            else:
                note_readback(value)
        elif kind == 'capture':
            _append_unique(missing_captures, os.path.abspath(value))
        else:
            _append_unique(missing_readbacks, os.path.abspath(value))
    try:
        children = sorted(os.listdir(boot))
    except OSError:
        children = []
    for child in children:
        candidate = os.path.join(boot, child)
        note_capture(candidate)
        note_readback(candidate)
    empty_captures = [path for path in empty_captures if path not in captures]
    empty_readbacks = [path for path in empty_readbacks if path not in readbacks]
    return {'boot': boot, 'tag': tag, 'meta': meta, 'env': env,
            'captures': captures, 'readbacks': readbacks,
            'empty_captures': empty_captures,
            'empty_readbacks': empty_readbacks,
            'missing_captures': missing_captures,
            'missing_readbacks': missing_readbacks,
            'suppressed': suppressed}


def analyse_arm(argument, root):
    resolved = resolve_arm(argument, root)
    if resolved is None:
        return {'error': 'arm evidence directory not found: %r (root %r)'
                         % (argument, root)}
    boot = resolved['boot']
    env = resolved['env']
    report = {'mode': 'arm', 'boot': boot, 'tag': resolved['tag'],
              'meta': resolved['meta'], 'switches': {},
              'switch_duplicates': resolved['meta']['env_duplicates']
              if resolved['meta'] else {},
              'suppressed': resolved['suppressed'],
              'summary': parse_summary(boot),
              'stderr': scan_stderr(boot),
              'empty': {'captures': resolved['empty_captures'],
                        'readbacks': resolved['empty_readbacks']},
              'missing': {'captures': resolved['missing_captures'],
                          'readbacks': resolved['missing_readbacks']},
              'captures': [], 'readbacks': [], 'notes': []}
    for switch in sorted(env):
        if switch.startswith('PALMETTO_'):
            report['switches'][switch] = env[switch]
    for directory in resolved['captures']:
        capture = analyse_capture(directory)
        if 'error' in capture:
            report['notes'].append(capture['error'])
            continue
        report['captures'].append(capture)
    for directory in resolved['readbacks']:
        readback = analyse_readback(directory)
        if 'error' in readback:
            report['notes'].append(readback['error'])
            continue
        report['readbacks'].append(readback)
    for path in report['missing']['captures']:
        report['notes'].append('PALMETTO_CAPTURE names a path that does not exist'
                               ' (no capture could be written): %s' % path)
    for path in report['missing']['readbacks']:
        report['notes'].append('PALMETTO_READBACK_PPM names a path that does not exist'
                               ' (no readback could be written): %s' % path)
    if not report['captures'] and 'capture' not in report['suppressed']:
        if report['empty']['captures']:
            for path in report['empty']['captures']:
                report['notes'].append('capture dir exists but holds no frame PPMs or index.txt: %s' % path)
        else:
            report['notes'].append('no capture directory with PPM frames was found')
    if not report['readbacks'] and 'readback' not in report['suppressed']:
        if report['empty']['readbacks']:
            for path in report['empty']['readbacks']:
                report['notes'].append('readback dir exists but holds no readback PPMs: %s' % path)
        else:
            report['notes'].append('no readback directory with PPM frames was found')
    return report



def analyse_images(directory):
    """Read a directory of PNG/PPM evidence (e.g. window screenshots) the same way.

    Window-grab arms write PNG screenshots of the title's own window instead of the
    presenter capture's PPM, so this mode reports the same per-frame statistics and
    classifications over either format.
    """
    if not os.path.isdir(directory):
        return {'error': 'image directory not found: %s' % directory}
    report = {'mode': 'images', 'directory': os.path.abspath(directory),
              'frames': [], 'errors': [], 'checks': []}
    names = sorted(name for name in os.listdir(directory)
                   if name.lower().endswith('.ppm') or name.lower().endswith('.png'))
    for name in names:
        frame = analyse_frame_file(os.path.join(directory, name))
        frame['class'] = classify_readback(frame)
        report['frames'].append(frame)
    for frame in report['frames']:
        if 'error' in frame:
            report['errors'].append({'file': frame['file'], 'error': frame['error']})
    usable = [frame for frame in report['frames'] if 'error' not in frame]
    counts = {}
    for frame in usable:
        counts[frame['class']] = counts.get(frame['class'], 0) + 1
    content = [frame for frame in usable if frame['class'] == 'content']
    summary = {'files': len(names), 'usable': len(usable),
               'black': counts.get('black', 0),
               'write_control': counts.get('write_control', 0),
               'uniform': counts.get('uniform', 0),
               'synthetic_four_band': counts.get('synthetic_four_band', 0),
               'content': counts.get('content', 0),
               'extents': sorted(set('%dx%d' % (f['width'], f['height']) for f in usable)),
               'any_content': bool(content),
               'best_frame': None, 'best_frame_path': None}
    if usable:
        best = max(usable, key=lambda f: (f['class'] == 'content',
                                          f['non_black'], sum(f['mean'])))
        summary['best_frame'] = best['file']
        summary['best_frame_path'] = best['path']
    report['summary'] = summary
    return report


def format_frame(frame, indent='  ', extra=''):
    if 'error' in frame:
        extent = ''
        if frame.get('width') and frame.get('height'):
            extent = ' %dx%d' % (frame['width'], frame['height'])
        return (indent + '%-42s%s  ERROR %s'
                % (frame['file'], extent, frame['error']))
    if frame['black']:
        status = 'BLACK'
    else:
        percent = (100.0 * frame['non_black'] / frame['pixels']) if frame['pixels'] else 0.0
        status = ('NON-BLACK %d/%d px (%.2f%%)'
                  % (frame['non_black'], frame['pixels'], percent))
    text = (indent + '%-42s %dx%d  mean rgb %.2f %.2f %.2f  min %d %d %d  max %d %d %d  %s'
            % (frame['file'], frame['width'], frame['height'],
               frame['mean'][0], frame['mean'][1], frame['mean'][2],
               frame['min'][0], frame['min'][1], frame['min'][2],
               frame['max'][0], frame['max'][1], frame['max'][2], status))
    if frame.get('uniform') and not frame['black']:
        text += '  uniform colour %d,%d,%d' % tuple(frame['colour'])
    if frame.get('synthetic'):
        text += '  [presenter synthetic four-band]'
    if extra:
        text += '  ' + extra
    return text


def render_checks(report, indent='  '):
    lines = []
    for check in report.get('checks', []):
        name = check.get('check')
        if name == 'index_vs_files':
            if check.get('index_entries') is None:
                lines.append(indent + 'index cross-check: '
                             + str(check.get('detail', 'no index.txt')))
                continue
            lines.append(indent + 'index cross-check: %d index entries, %d frame files;'
                         ' index without file: %s; file without index: %s; malformed lines: %d'
                         % (check['index_entries'], check['frame_files'],
                            check['index_without_file'] or 'none',
                            check['file_without_index'] or 'none',
                            len(check.get('malformed') or [])))
        elif name == 'index_vs_computed':
            mismatched = check.get('mismatched') or []
            if mismatched:
                lines.append(indent + 'index mean/extent cross-check: %d mismatched (tol %.2f)'
                             % (len(mismatched), check['tolerance']))
                for item in mismatched[:5]:
                    lines.append(indent + '  frame %s: %s'
                                 % (item['frame'], '; '.join(item['problems'])))
            else:
                lines.append(indent + 'index mean/extent cross-check: all agree (tol %.2f)'
                             % check['tolerance'])
    for error in (report.get('errors') or [])[:8]:
        lines.append(indent + 'ERROR %s: %s'
                     % (error.get('file', '?'), error.get('error', '?')))
    return lines


def render_capture(report, indent=''):
    lines = [indent + 'capture dir: %s' % report['directory']]
    index = report.get('index')
    if index:
        lines.append(indent + '  index.txt: %d lines, sha256 %s'
                     % (index['entries'], index.get('sha256', '?')))
    for frame in report['frames']:
        lines.append(format_frame(frame, indent + '  '))
    lines.extend(render_checks(report, indent + '  '))
    summary = report.get('summary') or {}
    if summary:
        lines.append(indent + '  summary: %d frame files, %d usable, %d black, %d non-black, %d presenter-synthetic;'
                     ' extents %s; best %s (mean %.2f %.2f %.2f)'
                     % (summary.get('frame_files', 0), summary.get('usable', 0),
                        summary.get('black', 0), summary.get('non_black', 0),
                        summary.get('synthetic_matches', 0),
                        ', '.join(summary.get('extents') or ['none']),
                        summary.get('best_frame'),
                        (summary.get('best_mean') or [0.0, 0.0, 0.0])[0],
                        (summary.get('best_mean') or [0.0, 0.0, 0.0])[1],
                        (summary.get('best_mean') or [0.0, 0.0, 0.0])[2]))
        if summary.get('frame_files', 0) == 0:
            lines.append(indent + '  verdict: no frame PPMs were written (empty or suppressed capture);'
                         ' no pixel is claimed')
        elif summary.get('any_non_black'):
            lines.append(indent + '  *** NON-BLACK CAPTURE FRAME(S): the presented frame is not black ***')
        else:
            lines.append(indent + '  verdict: every captured presented frame is black (all-zero RGB)')
    else:
        lines.append(indent + '  ERRORS: %d frame file(s) could not be read'
                     % len(report.get('errors') or []))
    return lines


def render_readback(report, indent=''):
    lines = [indent + 'readback dir: %s' % report['directory']]
    for frame in report['frames']:
        lines.append(format_frame(frame, indent + '  ', '[%s]' % frame.get('class', '?')))
    lines.extend(render_checks(report, indent + '  '))
    summary = report.get('summary') or {}
    if summary:
        lines.append(indent + '  summary: %d PPM files, %d black, %d write-control (51,102,204),'
                     ' %d uniform, %d synthetic four-band, %d content; extents %s; best %s'
                     % (summary.get('files', 0), summary.get('black', 0),
                        summary.get('write_control', 0), summary.get('uniform', 0),
                        summary.get('synthetic_four_band', 0),
                        summary.get('content', 0),
                        ', '.join(summary.get('extents') or ['none']),
                        summary.get('best_frame')))
        if summary.get('files', 0) == 0:
            lines.append(indent + '  verdict: no readback PPMs were written (empty readback dir);'
                         ' no pixel is claimed')
        elif summary.get('any_content'):
            lines.append(indent + '  *** NON-BLACK READBACK TARGET(S): title content candidate ***')
        else:
            lines.append(indent + '  verdict: no non-black title-content readback'
                         ' (black and/or the layer write-control colour only)')
    else:
        lines.append(indent + '  ERRORS: %d PPM file(s) could not be read'
                     % len(report.get('errors') or []))
    return lines




def render_images(report, indent=''):
    lines = [indent + 'images dir: %s' % report['directory']]
    for frame in report['frames']:
        lines.append(format_frame(frame, indent + '  ', '[%s]' % frame.get('class', '?')))
    lines.extend(render_checks(report, indent + '  '))
    summary = report.get('summary') or {}
    if summary:
        lines.append(indent + '  summary: %d image files, %d black, %d write-control (51,102,204),'
                     ' %d uniform, %d synthetic four-band, %d content; extents %s; best %s'
                     % (summary.get('files', 0), summary.get('black', 0),
                        summary.get('write_control', 0), summary.get('uniform', 0),
                        summary.get('synthetic_four_band', 0),
                        summary.get('content', 0),
                        ', '.join(summary.get('extents') or ['none']),
                        summary.get('best_frame')))
        if summary.get('files', 0) == 0:
            lines.append(indent + '  verdict: no image files were written; no pixel is claimed')
        elif summary.get('any_content'):
            lines.append(indent + '  *** NON-BLACK IMAGE(S): window/presented content candidate ***')
        else:
            lines.append(indent + '  verdict: no non-black window content'
                         ' (black and/or the layer write-control colour only)')
    else:
        lines.append(indent + '  ERRORS: %d image file(s) could not be read'
                     % len(report.get('errors') or []))
    return lines


def render_arm(report, indent=''):
    lines = [indent + 'arm evidence: %s' % report['boot'],
             indent + '  tag: %s' % report.get('tag')]
    meta = report.get('meta')
    if meta:
        values = meta.get('values') or {}
        for key in ('exe', 'exe_sha256', 'timeout', 'light', 'start', 'end'):
            if key in values:
                lines.append(indent + '  %s: %s' % (key, values[key]))
    switches = report.get('switches') or {}
    if switches:
        lines.append(indent + '  switches (effective, last value wins):')
        for name in sorted(switches):
            suffix = ''
            if report.get('switch_duplicates', {}).get(name):
                suffix = '  [overridden %d time(s)]' % report['switch_duplicates'][name]
            lines.append(indent + '    %s = %s%s' % (name, switches[name], suffix))
    else:
        lines.append(indent + '  switches: none parsed (no meta.txt env line)')
    summary = report.get('summary') or {}
    if summary.get('verdict'):
        lines.append(indent + '  verdict: %s' % summary['verdict'])
    if summary.get('wall'):
        lines.append(indent + '  wall: %s' % summary['wall'])
    if summary.get('exceptions'):
        lines.append(indent + '  exceptions: %s' % summary['exceptions'])
    if summary.get('d3d12'):
        lines.append(indent + '  d3d12: %s' % summary['d3d12'])
    stderr = report.get('stderr') or {}
    lines.append(indent + '  stderr scan (%d bytes): device-lost %d, present-failed %d,'
                 ' capture lines %d, readback-ppm lines %d'
                 % (stderr.get('bytes', 0), stderr.get('device_lost', 0),
                    stderr.get('present_failed', 0), stderr.get('capture_lines', 0),
                    stderr.get('readback_lines', 0)))
    if stderr.get('device_lost_first'):
        lines.append(indent + '    first: %s' % stderr['device_lost_first'])
    if stderr.get('cannot_write'):
        lines.append(indent + '  capture-write failures: %d (the capture directory was not writable)'
                     % stderr['cannot_write'])
    if stderr.get('last_present_attest'):
        lines.append(indent + '  last present attest: %s' % stderr['last_present_attest'])
    for note in report.get('notes') or []:
        lines.append(indent + '  note: %s' % note)
    for key, text in sorted((report.get('suppressed') or {}).items()):
        lines.append(indent + '  suppressed %s: %s' % (key, text))
    for capture in report.get('captures') or []:
        lines.extend(render_capture(capture, indent + '  '))
    for readback in report.get('readbacks') or []:
        lines.extend(render_readback(readback, indent + '  '))
    return lines


def render_text(report):
    if report['mode'] == 'capture':
        lines = render_capture(report)
    elif report['mode'] == 'readback':
        lines = render_readback(report)
    elif report['mode'] == 'images':
        lines = render_images(report)
    else:
        lines = render_arm(report)
    output = report.get('output')
    if output:
        lines.append('  output dir: %s' % output['directory'])
        for item in output.get('written') or []:
            if item.get('bytes'):
                lines.append('    wrote %s (%d bytes)' % (item.get('path'), item['bytes']))
            else:
                lines.append('    wrote %s' % item.get('path'))
    return '\n'.join(lines)


def one_line_summary(report):
    parts = ['arm %s' % report.get('tag')]
    for capture in report.get('captures') or []:
        summary = capture['summary']
        parts.append('capture %d frames: %d black / %d non-black (%d synthetic), best %s'
                     % (summary['frame_files'], summary['black'], summary['non_black'],
                        summary['synthetic_matches'], summary['best_frame']))
    for readback in report.get('readbacks') or []:
        summary = readback['summary']
        parts.append('readback %d files: %d black / %d write-control / %d content'
                     % (summary['files'], summary['black'],
                        summary['write_control'], summary['content']))
    if not report.get('captures') and not report.get('readbacks'):
        parts.append('no PPM evidence' + (' (suppressed by design)' if report.get('suppressed') else ''))
    return '; '.join(parts)


def default_output_dir(report):
    if report['mode'] == 'arm':
        return os.path.join(report['boot'], 'frame_analysis')
    return os.path.join(report['directory'], 'frame_analysis')


def write_outputs(report, out_dir, png_max_dim=0):
    os.makedirs(out_dir, exist_ok=True)
    sources = []
    if report['mode'] in ('capture', 'readback', 'images'):
        best = (report.get('summary') or {}).get('best_frame_path')
        if best:
            sources.append(('best_frame.png', best))
    elif report['mode'] == 'arm':
        for index, capture in enumerate(report.get('captures') or [], 1):
            best = (capture.get('summary') or {}).get('best_frame_path')
            if best:
                sources.append(('best_frame_capture_%d.png' % index, best))
        for index, readback in enumerate(report.get('readbacks') or [], 1):
            best = (readback.get('summary') or {}).get('best_frame_path')
            if best:
                sources.append(('best_frame_readback_%d.png' % index, best))
    written = []
    errors = []
    for name, source in sources:
        image = read_image(source)
        if 'error' in image:
            errors.append({'source': source, 'error': image['error']})
            continue
        try:
            if source.lower().endswith('.png'):
                with open(source, 'rb') as handle:
                    payload = handle.read()
                path = os.path.join(out_dir, name)
                with open(path, 'wb') as handle:
                    handle.write(payload)
                written.append({'path': path, 'width': image['width'],
                                'height': image['height'], 'bytes': len(payload),
                                'copied_png': True})
            else:
                written.append(write_png(os.path.join(out_dir, name),
                                         image['width'], image['height'],
                                         image['data'], png_max_dim))
        except OSError as error:
            errors.append({'source': source, 'error': str(error)})
    text_path = os.path.join(out_dir, 'summary.txt')
    json_path = os.path.join(out_dir, 'summary.json')
    report['output'] = {'directory': os.path.abspath(out_dir),
                        'written': written + [{'path': os.path.abspath(text_path)},
                                               {'path': os.path.abspath(json_path)}],
                        'errors': errors}
    text = render_text(report) + '\n'
    with open(text_path, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(text)
    with open(json_path, 'w', encoding='utf-8', newline='\n') as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
        handle.write('\n')
    return report['output']


def _write_ppm(path, width, height, pixels):
    with open(path, 'wb') as handle:
        handle.write(('P6\n%d %d\n255\n' % (width, height)).encode('ascii'))
        handle.write(bytes(pixels))


def _upscale(pattern, src_w, src_h, dst_w, dst_h):
    out = bytearray(dst_w * dst_h * 3)
    for y in range(dst_h):
        sy = (y * src_h) // dst_h
        for x in range(dst_w):
            sx = (x * src_w) // dst_w
            source = (sy * src_w + sx) * 3
            target = (y * dst_w + x) * 3
            out[target:target + 3] = pattern[source:source + 3]
    return bytes(out)


def _index_line(number, fmt, width, height, mean):
    return ('frame %d format %d (VK_FORMAT_B8G8R8A8_UNORM) extent %dx%d mean rgb %.2f %.2f %.2f'
            % (number, fmt, width, height, mean[0], mean[1], mean[2]))


def _write_index(path, entries):
    with open(path, 'w', encoding='utf-8', newline='\n') as handle:
        for entry in entries:
            handle.write(entry + '\n')


def _find_check(report, name):
    for check in report.get('checks') or []:
        if check.get('check') == name:
            return check
    return {}


def self_test():
    import shutil
    import tempfile
    root = tempfile.mkdtemp(prefix='frame_analysis_selftest_')
    checks = []

    def check(name, condition, detail=''):
        ok = bool(condition)
        checks.append((name, ok))
        if ok:
            print('PASS ' + name)
        else:
            print('FAIL ' + name + ((' :: ' + str(detail)) if detail else ''))

    try:
        source_w, source_h = 16, 9
        pattern = bytearray()
        for _y in range(source_h):
            for x in range(source_w):
                pattern.extend(SYNTHETIC_BANDS[x // 4])
        width, height = 160, 90
        frame = _upscale(pattern, source_w, source_h, width, height)
        expected_mean = [sum(frame[c::3]) / float(width * height) for c in range(3)]

        capture = os.path.join(root, 'capture-selftest')
        os.makedirs(capture)
        index_entries = []
        for number in (1, 2, 3):
            _write_ppm(os.path.join(capture, 'frame_%05d.ppm' % number),
                       width, height, frame)
            index_entries.append(_index_line(number, 44, width, height, expected_mean))
        _write_index(os.path.join(capture, 'index.txt'), index_entries)
        report = analyse_capture(capture)
        check('capture: three frame files found',
              report['summary']['frame_files'] == 3, report.get('summary'))
        check('capture: all three frames non-black',
              report['summary']['non_black'] == 3 and report['summary']['any_non_black'])
        check('capture: presenter synthetic four-band probe matches 3/3',
              report['summary']['synthetic_matches'] == 3)
        first = report['frames'][0]
        check('stats: exact whole-frame mean of the four bands',
              all(abs(first['mean'][i] - expected_mean[i]) < 1e-9 for i in range(3)),
              (first['mean'], expected_mean))
        check('stats: min/max are the band colour extremes',
              first['min'] == [51, 51, 102] and first['max'] == [255, 255, 255],
              (first['min'], first['max']))
        check('stats: every pixel is non-black',
              first['non_black'] == first['pixels'])
        check('capture: index file cross-check ok',
              _find_check(report, 'index_vs_files').get('ok') is True)
        check('capture: index mean/extent cross-check ok',
              _find_check(report, 'index_vs_computed').get('ok') is True)

        black_dir = os.path.join(root, 'capture-black')
        os.makedirs(black_dir)
        black = bytes(8 * 6 * 3)
        for number in (1, 2):
            _write_ppm(os.path.join(black_dir, 'frame_%05d.ppm' % number), 8, 6, black)
        _write_index(os.path.join(black_dir, 'index.txt'),
                     [_index_line(1, 44, 8, 6, [0.0, 0.0, 0.0]),
                      _index_line(2, 44, 8, 6, [0.0, 0.0, 0.0])])
        black_report = analyse_capture(black_dir)
        check('black capture: both frames flagged black',
              black_report['summary']['black'] == 2
              and not black_report['summary']['any_non_black'])
        check('black capture: no synthetic match',
              black_report['summary']['synthetic_matches'] == 0)

        truncated_dir = os.path.join(root, 'capture-truncated')
        os.makedirs(truncated_dir)
        truncated_path = os.path.join(truncated_dir, 'frame_00001.ppm')
        full = ('P6\n160 90\n255\n').encode('ascii') + frame
        with open(truncated_path, 'wb') as handle:
            handle.write(full[:-9])
        truncated_report = analyse_capture(truncated_dir)
        check('truncated PPM: reported as an error, not as black',
              len(truncated_report['errors']) == 1
              and 'truncated' in truncated_report['errors'][0]['error'])

        mismatch_dir = os.path.join(root, 'capture-mismatch')
        os.makedirs(mismatch_dir)
        _write_ppm(os.path.join(mismatch_dir, 'frame_00001.ppm'), width, height, frame)
        _write_ppm(os.path.join(mismatch_dir, 'frame_00002.ppm'), 8, 6, black)
        _write_index(os.path.join(mismatch_dir, 'index.txt'),
                     [_index_line(1, 44, 160, 90, [0.0, 0.0, 0.0]),
                      _index_line(99, 44, 160, 90, [1.0, 2.0, 3.0])])
        mismatch_report = analyse_capture(mismatch_dir)
        check('index mismatch: index-without-file reported',
              _find_check(mismatch_report, 'index_vs_files').get('index_without_file') == [99],
              _find_check(mismatch_report, 'index_vs_files'))
        check('index mismatch: file-without-index reported',
              _find_check(mismatch_report, 'index_vs_files').get('file_without_index') == [2])
        check('index mismatch: wrong mean reported',
              not _find_check(mismatch_report, 'index_vs_computed').get('ok'))

        readback_dir = os.path.join(root, 'readback-selftest')
        os.makedirs(readback_dir)
        control = bytes(WRITE_CONTROL) * (16 * 16)
        _write_ppm(os.path.join(readback_dir,
                                'readback_draw6382_res1889f6b6720_16x16.ppm'),
                   16, 16, control)
        _write_ppm(os.path.join(readback_dir,
                                'readback_draw6383_res1889f6b6720_16x16.ppm'),
                   16, 16, bytes(16 * 16 * 3))
        readback = analyse_readback(readback_dir)
        check('readback: exact write-control colour classified',
              readback['summary']['write_control'] == 1, readback['summary'])
        check('readback: black target classified',
              readback['summary']['black'] == 1)
        check('readback: no title-content flag',
              not readback['summary']['any_content'])
        parsed = readback['frames'][0]['readback'] or {}
        check('readback: file name parsed (draw/resource/extent)',
              parsed.get('draw') == 6382 and parsed.get('resource') == '1889f6b6720'
              and parsed.get('width') == 16)

        png_path = os.path.join(root, 'best.png')
        png = write_png(png_path, width, height, frame)
        with open(png_path, 'rb') as handle:
            png_bytes = handle.read()
        check('png: signature', png_bytes[:8] == PNG_SIGNATURE)
        check('png: IHDR extent', struct.unpack('>II', png_bytes[16:24]) == (width, height))
        position = 8
        idat = b''
        while position + 8 <= len(png_bytes):
            length = struct.unpack('>I', png_bytes[position:position + 4])[0]
            tag = png_bytes[position + 4:position + 8]
            if tag == b'IDAT':
                idat = png_bytes[position + 8:position + 8 + length]
            position += 12 + length
        expected_rows = b''.join(bytes(1) + frame[y * width * 3:(y + 1) * width * 3]
                                 for y in range(height))
        check('png: decoded scanlines equal the PPM RGB',
              zlib.decompress(idat) == expected_rows)
        small = write_png(os.path.join(root, 'small.png'), width, height, frame, 64)
        check('png: --png-max-dim downsamples',
              small['width'] == 54 and small['height'] == 30, small)


        def encode_png(path, width, height, rgb, filters):
            channels = 3
            stride = width * channels
            raw = bytearray()
            previous = bytes(stride)
            for y in range(height):
                row = rgb[y * stride:(y + 1) * stride]
                kind = filters[y % len(filters)]
                residual = bytearray(stride)
                for i in range(stride):
                    left = row[i - channels] if i >= channels else 0
                    up = previous[i]
                    upleft = previous[i - channels] if i >= channels else 0
                    if kind == 0:
                        predictor = 0
                    elif kind == 1:
                        predictor = left
                    elif kind == 2:
                        predictor = up
                    elif kind == 3:
                        predictor = (left + up) >> 1
                    else:
                        prediction = left + up - upleft
                        distances = (abs(prediction - left), abs(prediction - up),
                                     abs(prediction - upleft))
                        predictor = (left, up, upleft)[distances.index(min(distances))]
                    residual[i] = (row[i] - predictor) & 0xFF
                raw.append(kind)
                raw.extend(residual)
                previous = row
            payload = (PNG_SIGNATURE
                       + _png_chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                       + _png_chunk(b'IDAT', zlib.compress(bytes(raw), 6))
                       + _png_chunk(b'IEND', b''))
            with open(path, 'wb') as handle:
                handle.write(payload)

        width_png, height_png = 12, 6
        rgb_png = bytearray()
        for y in range(height_png):
            for x in range(width_png):
                rgb_png.extend(((x * 17) & 0xFF, (y * 41) & 0xFF, (x * 7 + y * 3) & 0xFF))
        filtered_path = os.path.join(root, 'filtered.png')
        encode_png(filtered_path, width_png, height_png, bytes(rgb_png), [0, 1, 2, 3, 4])
        decoded = read_png(filtered_path)
        check('png reader: None/Sub/Up/Average/Paeth filters decode exactly',
              decoded.get('data') == bytes(rgb_png), decoded.get('error'))
        black_png_path = os.path.join(root, 'black.png')
        encode_png(black_png_path, 10, 5, bytes(10 * 5 * 3), [0, 1, 2, 4])
        black_png = read_png(black_png_path)
        black_stats = pixel_stats(black_png['data'], black_png['width'], black_png['height'])
        check('png reader: all-black PNG takes the zero-residual fast path',
              black_stats['black'] and 'fast path' in str(black_png.get('filters')))
        images_dir = os.path.join(root, 'images')
        os.makedirs(images_dir)
        encode_png(os.path.join(images_dir, 'shot_001.png'), width_png, height_png,
                   bytes(rgb_png), [0])
        encode_png(os.path.join(images_dir, 'shot_002.png'), 10, 5, bytes(10 * 5 * 3), [0])
        images = analyse_images(images_dir)
        check('images mode: PNG screenshots classified (content + black)',
              images['summary']['content'] == 1 and images['summary']['black'] == 1,
              images.get('summary'))
        check('images mode: best frame is the non-black screenshot',
              images['summary']['best_frame'] == 'shot_001.png')
        images_out = write_outputs(images, os.path.join(root, 'analysis-images'), 0)
        copied = [item for item in images_out['written']
                  if item.get('copied_png')]
        check('images mode: best PNG copied to the output dir',
              len(copied) == 1 and os.path.isfile(copied[0]['path']))

        arm_root = os.path.join(root, 'fsrstable')
        os.makedirs(arm_root)
        boot = os.path.join(arm_root, 'boot-tag1')
        arm_capture = os.path.join(arm_root, 'capture-tag1')
        arm_readback = os.path.join(arm_root, 'readback-tag1')
        for directory in (boot, arm_capture, arm_readback):
            os.makedirs(directory)
        _write_ppm(os.path.join(arm_capture, 'frame_00001.ppm'), width, height, frame)
        _write_index(os.path.join(arm_capture, 'index.txt'),
                     [_index_line(1, 44, width, height, expected_mean)])
        _write_ppm(os.path.join(arm_readback,
                                'readback_draw6382_res1889f6b6720_16x16.ppm'),
                   16, 16, control)
        meta_lines = ['tag=tag1', 'exe_sha256=0123456789ABCDEF', 'timeout=30',
                      'env=PALMETTO_SHADERS=x PALMETTO_CAPTURE=%s'
                      ' PALMETTO_READBACK_PPM=%s PALMETTO_PRESENT_SYNTHETIC=8'
                      ' PALMETTO_CAPTURE_EVERY=1 PALMETTO_CAPTURE_LIMIT=8'
                      % (arm_capture, arm_readback)]
        with open(os.path.join(boot, 'meta.txt'), 'w', encoding='utf-8', newline='\n') as handle:
            handle.write('\n'.join(meta_lines) + '\n')
        with open(os.path.join(boot, 'summary_console.txt'), 'w', encoding='utf-8',
                  newline='\n') as handle:
            handle.write('VERDICT: ALIVE 30.0 s\nfirst exception: none recorded\n')
        with open(os.path.join(boot, 'stderr.log'), 'w', encoding='utf-8', newline='\n') as handle:
            handle.write('[rt] replay: the Vulkan device was lost\n')
            handle.write('[rt] present: capture frame_00001.ppm (160x90, mean rgb 140.25 153.00 165.75)\n')
        arm = analyse_arm('tag1', arm_root)
        check('arm: resolved boot dir', arm.get('boot') == boot, arm.get('error'))
        check('arm: sibling capture dir resolved', len(arm.get('captures') or []) == 1)
        check('arm: sibling readback dir resolved', len(arm.get('readbacks') or []) == 1)
        check('arm: verdict parsed', (arm.get('summary') or {}).get('verdict') == 'ALIVE 30.0 s')
        check('arm: device-lost line counted', (arm.get('stderr') or {}).get('device_lost') == 1)
        check('arm: capture line counted', (arm.get('stderr') or {}).get('capture_lines') == 1)
        check('arm: switch parsed', (arm.get('switches') or {}).get('PALMETTO_PRESENT_SYNTHETIC') == '8')
        output = write_outputs(arm, os.path.join(root, 'analysis-tag1'), 64)
        names = [os.path.basename(item.get('path', '')) for item in output.get('written') or []]
        check('arm: outputs written (summary + best PNGs)',
              'summary.txt' in names and 'summary.json' in names
              and any(name.startswith('best_frame_capture_') for name in names)
              and any(name.startswith('best_frame_readback_') for name in names), names)

        boot2 = os.path.join(arm_root, 'boot-tag2')
        os.makedirs(boot2)
        with open(os.path.join(boot2, 'meta.txt'), 'w', encoding='utf-8', newline='\n') as handle:
            handle.write('tag=tag2\n')
            handle.write('env=PALMETTO_CAPTURE=%s PALMETTO_CAPTURE= PALMETTO_READBACK_PPM=\n'
                         % arm_capture)
        arm2 = analyse_arm('tag2', arm_root)
        check('arm: empty switch value marked as suppressed by design',
              'capture' in (arm2.get('suppressed') or {}), arm2.get('suppressed'))
        check('arm: one-line summary says suppressed',
              'suppressed by design' in one_line_summary(arm2), one_line_summary(arm2))

        empty_dir = os.path.join(root, 'capture-empty')
        os.makedirs(empty_dir)
        empty_report = analyse_capture(empty_dir)
        check('capture: empty dir reports zero frames and no black claim',
              empty_report['summary']['frame_files'] == 0
              and not empty_report['summary']['any_non_black'])
        check('capture: empty dir verdict claims no pixel',
              'no frame PPMs were written' in '\n'.join(render_capture(empty_report)))

        boot3 = os.path.join(arm_root, 'boot-tag3')
        empty_capture = os.path.join(arm_root, 'capture-tag3')
        os.makedirs(boot3)
        os.makedirs(empty_capture)
        with open(os.path.join(boot3, 'meta.txt'), 'w', encoding='utf-8', newline='\n') as handle:
            handle.write('tag=tag3\n')
            handle.write('env=PALMETTO_CAPTURE=%s\n' % empty_capture)
        arm3 = analyse_arm('tag3', arm_root)
        check('arm: empty capture dir listed as empty',
              empty_capture in (arm3.get('empty') or {}).get('captures', []),
              arm3.get('empty'))
        check('arm: empty capture dir yields no black verdict',
              not arm3.get('captures') and 'no PPM evidence' in one_line_summary(arm3))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    failed = [name for name, ok in checks if not ok]
    print('self-test: %d checks, %d failed' % (len(checks), len(failed)))
    return 0 if not failed else 1


def main(argv):
    parser = argparse.ArgumentParser(
        description='Palmetto R22 capture/readback evidence reader (PPM stats, best frame PNG, arm summary)')
    subparsers = parser.add_subparsers(dest='command')
    for name in ('arm', 'capture', 'readback', 'images'):
        sub = subparsers.add_parser(name)
        sub.add_argument('target', nargs='?')
        sub.add_argument('--root', default=None,
                         help='fsrstable-style directory holding boot-<tag>/capture-<tag>/readback-<tag>')
        sub.add_argument('--out', default=None, help='analysis output directory')
        sub.add_argument('--no-write', action='store_true', help='print only, write no PNG/report')
        sub.add_argument('--png-max-dim', type=int, default=0,
                         help='downsample the PNG to at most this width/height (0 = keep size)')
        sub.add_argument('--print-json', action='store_true', help='also print the JSON report')
        sub.add_argument('--fail-if-black', action='store_true',
                         help='exit 2 when every captured frame is black')
        sub.add_argument('--fail-if-nonblack', action='store_true',
                         help='exit 3 when any captured frame is non-black')
        sub.add_argument('--quiet', action='store_true', help='one-line summary only (arm mode)')
    subparsers.add_parser('self-test', help='run the synthetic PPM/PNG/arm self-test')
    args = parser.parse_args(argv)
    if args.command == 'self-test':
        return self_test()
    if not args.command:
        parser.print_help()
        return 1
    if not args.target:
        print('error: a capture/readback directory or arm tag is required')
        return 1
    if args.command == 'capture':
        report = analyse_capture(args.target)
    elif args.command == 'readback':
        report = analyse_readback(args.target)
    elif args.command == 'images':
        report = analyse_images(args.target)
    else:
        report = analyse_arm(args.target, args.root)
    if 'error' in report:
        print('error: %s' % report['error'])
        return 1
    out_dir = args.out or default_output_dir(report)
    if not args.no_write:
        write_outputs(report, out_dir, args.png_max_dim)
    if args.quiet and report['mode'] == 'arm':
        print(one_line_summary(report))
    elif not args.quiet:
        print(render_text(report))
    if args.print_json:
        print(json.dumps(report, indent=2, sort_keys=True))
    captures = []
    if report['mode'] == 'capture':
        captures = [report]
    elif report['mode'] == 'arm':
        captures = report.get('captures') or []
    any_non_black = any(capture['summary']['any_non_black'] for capture in captures)
    frames_written = sum(int((capture.get('summary') or {}).get('frame_files', 0))
                         for capture in captures)
    if args.fail_if_nonblack and any_non_black:
        print('GATE: --fail-if-nonblack fired (a non-black captured frame exists)')
        return 3
    if args.fail_if_black and frames_written and not any_non_black:
        print('GATE: --fail-if-black fired (every captured frame is black)')
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))