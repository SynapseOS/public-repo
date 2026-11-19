# interpreter.py: Заменить путь загрузчика в PT_INTERP без перестройки ELF
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: серверные пакеты, первое и последнее изменение: 13.11.2026
# Статус доступа: общедоступный

from pathlib import Path
from struct import unpack_from
import sys


def set_interpreter(binary):
    with binary.open('r+b') as stream:
        header = stream.read(64)
        if header[:6] != b'\x7fELF\x02\x01' or unpack_from('<H', header, 18)[0] != 62:
            raise ValueError('Expected an x86_64 ELF')

        offset = unpack_from('<Q', header, 32)[0]
        stride, count = unpack_from('<HH', header, 54)
        for index in range(count):
            stream.seek(offset + index * stride)
            entry = stream.read(stride)
            if unpack_from('<I', entry)[0] != 3:
                continue

            start = unpack_from('<Q', entry, 8)[0]
            length = unpack_from('<Q', entry, 32)[0]
            stream.seek(start)
            if stream.read(length) != b'/lib64/ld-linux-x86-64.so.2\0':
                raise ValueError('Unexpected PT_INTERP')

            stream.seek(start)
            stream.write(b'/lib/ld-linux-x86-64.so.2\0'.ljust(length, b'\0'))
            return

        raise ValueError('Missing PT_INTERP')


if __name__ == '__main__':
    set_interpreter(Path(sys.argv[1]))
