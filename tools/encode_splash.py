"""Build-only H.264 packaging; source PNGs are read, never rewritten."""
from pathlib import Path
import hashlib
import json
import re
import struct
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
frames = sorted((root / 'graphics/splash_screen').glob('frame_*.png'))
assert frames, 'No splash PNG source frames'
settings = ['-an', '-c:v', 'libx264', '-preset', 'slow', '-tune', 'animation',
            '-crf', '12', '-profile:v', 'high', '-pix_fmt', 'yuv420p',
            '-vf', 'scale=in_range=full:out_range=tv:out_color_matrix=bt709',
            '-color_range', 'tv', '-colorspace', 'bt709', '-color_primaries', 'bt709',
            '-color_trc', 'bt709', '-g', '30', '-threads', '4', '-movflags', '+faststart']
fingerprint = hashlib.sha256(json.dumps(settings).encode())
digests = {}
dimensions = None
for index, frame in enumerate(frames, 1):
    assert re.fullmatch(rf'frame_{index:04d}\.png', frame.name), 'Splash frames must be contiguous from 0001'
    data = frame.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n' and data[12:16] == b'IHDR', 'Invalid source PNG'
    size = struct.unpack('>II', data[16:24])
    dimensions = dimensions or size
    assert size == dimensions and all(v % 2 == 0 for v in size), 'Inconsistent/odd video dimensions'
    digests[frame.name] = hashlib.sha256(data).hexdigest()
    fingerprint.update(frame.name.encode())
    fingerprint.update(data)
key = fingerprint.hexdigest()
movie = output / 'splash.mp4'
manifest = output / 'splash-video.json'
cached = json.loads(manifest.read_text()) if manifest.exists() else {}
if not (movie.exists() and cached.get('input_sha256') == key and
        cached.get('video_sha256') == hashlib.sha256(movie.read_bytes()).hexdigest()):
    temporary = output / 'splash.encoding.mp4'
    try:
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-framerate', '30',
                        '-start_number', '1', '-i', str(frames[0].parent / 'frame_%04d.png'),
                        '-frames:v', str(len(frames)), *settings, str(temporary)], check=True)
        probe = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
            '-show_entries', 'stream=codec_name,profile,pix_fmt,width,height,nb_frames,r_frame_rate',
            '-of', 'json', str(temporary)]))['streams'][0]
        assert probe['codec_name'] == 'h264' and probe['profile'] == 'High'
        assert probe['pix_fmt'] == 'yuv420p' and probe['r_frame_rate'] == '30/1'
        assert (probe['width'], probe['height']) == dimensions and int(probe['nb_frames']) == len(frames)
        # Verify encoding never changed any of the supplied bitmaps.
        assert all(hashlib.sha256(p.read_bytes()).hexdigest() == digests[p.name] for p in frames)
        temporary.replace(movie)
        manifest.write_text(json.dumps({'input_sha256': key, 'source_sha256': digests,
            'video_sha256': hashlib.sha256(movie.read_bytes()).hexdigest(),
            'frames': len(frames), 'fps': 30, 'size': dimensions, 'settings': settings}, indent=2) + '\n')
    finally:
        temporary.unlink(missing_ok=True)
# One lossless poster survives as a decoder fallback, not a PNG animation sequence.
(output / 'splash_frames.rcinc').write_text(
    'IDR_SPLASH_POSTER RCDATA "graphics/splash_screen/frame_0001.png"\n'
    'IDR_SPLASH_VIDEO RCDATA "build/generated/splash.mp4"\n')
print(f'Embedded splash: {len(frames)} frames, {dimensions[0]}x{dimensions[1]}, 30 FPS; '
      f'H.264 CRF 12 = {movie.stat().st_size:,} bytes, plus original poster.')
