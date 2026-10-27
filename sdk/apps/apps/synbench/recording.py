# recording.py: Запись настоящего экрана QEMU через QMP и ffmpeg
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux ABI
# Подсистема: видеозапись сравнительных измерений
# Первое изменение: 19.10.2026, последнее изменение: 20.10.2026
# Доступ: общедоступный

import json
import socket
import subprocess
import threading
import time


class ScreenRecorder:
    """Сохранять кадры QEMU без перерисовки или подмены вывода гостя."""

    def __init__(self, qmp_path, directory, fps=2):
        self.directory = directory.resolve()
        self.fps = fps
        self.frames = 0
        self.observations = []
        self.errors = []
        self.stop_event = threading.Event()
        self.rpc_lock = threading.Lock()
        self.connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.connection.settimeout(5)
        try:
            self.connection.connect(qmp_path)
            self.stream = self.connection.makefile('rwb', buffering=0)
            greeting = json.loads(self.stream.readline())
            if 'QMP' not in greeting:
                raise ValueError('QMP greeting is missing')
            self.rpc('qmp_capabilities')
        except BaseException:
            self.connection.close()
            raise
        self.log = (self.directory / 'ffmpeg.log').open('xb')
        try:
            self.encoder = subprocess.Popen([
                'ffmpeg', '-hide_banner', '-loglevel', 'warning', '-n',
                '-f', 'image2pipe', '-vcodec', 'ppm', '-framerate', str(fps), '-i', '-',
                '-vf', ('scale=1920:1080:force_original_aspect_ratio=decrease:flags=neighbor,'
                        'pad=1920:1080:(ow-iw)/2:(oh-ih)/2'), '-c:v', 'libx264',
                '-preset', 'ultrafast', '-crf', '18', '-threads', '1',
                '-pix_fmt', 'yuv420p', '-movflags', '+faststart', str(self.directory / 'console.mp4')],
                stdin=subprocess.PIPE, stdout=self.log, stderr=self.log)
        except BaseException:
            self.stream.close()
            self.connection.close()
            self.log.close()
            raise
        self.started = time.monotonic()
        self.thread = threading.Thread(target=self.record, daemon=True)

    def rpc(self, name, arguments=None):
        """Получить ответ на одну команду, пропуская асинхронные события QMP."""
        with self.rpc_lock:
            request = {'execute': name}
            if arguments is not None:
                request['arguments'] = arguments
            self.stream.write(json.dumps(request).encode() + b'\n')
            while True:
                line = self.stream.readline()
                if not line:
                    raise EOFError('QMP connection closed')
                answer = json.loads(line)
                if 'error' in answer:
                    raise ValueError('QMP error: ' + str(answer['error']))
                if 'return' in answer:
                    return answer['return']

    def start(self):
        """Начать запись перед запуском команды в госте."""
        self.thread.start()

    def capture(self):
        """Прочитать реальный framebuffer гостя из QEMU."""
        path = self.directory / 'frame.ppm'
        self.rpc('screendump', {'filename': str(path)})
        return path.read_bytes()

    def emit(self, frame):
        """Передать один неизменённый кадр кодировщику."""
        self.encoder.stdin.write(frame)
        self.encoder.stdin.flush()
        self.frames += 1

    def record(self):
        """Удерживать предыдущий кадр при задержке съёмки, сохраняя реальное время."""
        previous = None
        try:
            while not self.stop_event.is_set():
                frame = self.capture()
                elapsed = time.monotonic() - self.started
                required = int(elapsed * self.fps) + 1
                held = 0
                while self.frames < required - 1:
                    self.emit(previous if previous is not None else frame)
                    held += 1
                self.emit(frame)
                previous = frame
                self.observations.append({'wall_seconds': elapsed, 'frames': self.frames,
                                          'held_frames': held})
                self.stop_event.wait(max(0, self.frames / self.fps - (time.monotonic() - self.started)))
            # Последний вывод остаётся на экране ещё одну секунду, это отмечено в метаданных
            final = self.capture()
            for _ in range(self.fps):
                self.emit(final)
        except (OSError, EOFError, ValueError) as error:
            self.errors.append(str(error))

    def stop(self):
        """Завершить видео и явно сохранить ошибки записи."""
        self.stop_event.set()
        self.thread.join(timeout=10)
        if self.thread.is_alive():
            self.errors.append('Recorder did not stop within 10 seconds')
            self.connection.shutdown(socket.SHUT_RDWR)
            self.thread.join(timeout=6)
        try:
            self.encoder.stdin.close()
        except BrokenPipeError:
            self.errors.append('ffmpeg input closed unexpectedly')
        try:
            code = self.encoder.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.encoder.kill()
            code = self.encoder.wait()
            self.errors.append('ffmpeg timed out')
        if code != 0:
            self.errors.append('ffmpeg exit code: ' + str(code))
        self.stream.close()
        self.connection.close()
        self.log.close()
        result = {'file': 'console.mp4', 'source': 'QEMU framebuffer via QMP screendump',
                  'fps': self.fps, 'frames': self.frames, 'errors': self.errors,
                  'final_hold_seconds': 1, 'observations': self.observations}
        (self.directory / 'video-timing.json').write_text(json.dumps(result, indent=2) + '\n')
        return result
