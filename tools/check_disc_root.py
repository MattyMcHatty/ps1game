"""Check that the ISO's boot files still live in the FIRST root-directory sector.

The PS1 boot ROM reads only the first 2048-byte sector of the root directory to
find SYSTEM.CNF (and through it HORROR.BIN). mkpsxiso sorts the root
alphabetically, so adding a file does not append a record — it INSERTS one,
shoving every later record forward. Push SYSTEM.CNF's record past offset 2048
and the console hangs at the PlayStation logo with no other symptom.

The root list in disc.xml is effectively full. Run this after ANY disc.xml
change; put new files in a subdirectory (TEX / SND) instead.

    py tools/check_disc_root.py [build/HORROR.bin]

Exits 1 if the disc will not boot.
"""
import struct
import sys

BOOT_FILES = (b'SYSTEM.CNF;1', b'HORROR.BIN;1')
RAW_SECTOR, RAW_HEADER, DATA = 2352, 24, 2048


def sectors(path):
    """Return a reader for the 2048 data bytes of sector N.

    mkpsxiso emits raw 2352-byte Mode-2 sectors (12-byte sync, 4-byte header,
    8-byte subheader), but accept a plain 2048-byte image too.
    """
    f = open(path, 'rb')
    head = f.read(12)
    raw = head[0] == 0 and head[1:11] == b'\xff' * 10
    size, off = (RAW_SECTOR, RAW_HEADER) if raw else (DATA, 0)

    def read(n):
        f.seek(n * size + off)
        return f.read(DATA)
    return read, raw


def records(read):
    """Yield (name, start_offset, end_offset) for every root-directory record."""
    pvd = read(16)
    if pvd[1:6] != b'CD001':
        sys.exit('not an ISO9660 image: no CD001 at sector 16')
    root = pvd[156:190]
    lba = struct.unpack('<I', root[2:6])[0]
    length = struct.unpack('<I', root[10:14])[0]
    data = b''.join(read(lba + i) for i in range((length + DATA - 1) // DATA))

    off = 0
    while off < length:
        rec_len = data[off]
        if rec_len == 0:                      # pad to the next sector
            off = (off // DATA + 1) * DATA
            continue
        id_len = data[off + 32]
        yield data[off + 33:off + 33 + id_len], off, off + rec_len
        off += rec_len


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'build/HORROR.bin'
    read, raw = sectors(path)
    print('%s (%s sectors)' % (path, 'raw 2352' if raw else 'iso 2048'))

    found, ok, last_in_first = {}, True, 0
    for name, start, end in records(read):
        if end <= DATA:
            last_in_first = max(last_in_first, end)
        if name in BOOT_FILES:
            found[name] = (start, end)

    for name in BOOT_FILES:
        if name not in found:
            print('  %-14s MISSING FROM ROOT' % name.decode())
            ok = False
            continue
        start, end = found[name]
        good = end <= DATA
        ok &= good
        print('  %-14s record [%d,%d)  %s'
              % (name.decode(), start, end,
                 'first sector, OK' if good
                 else '*** PAST %d: WILL NOT BOOT ***' % DATA))

    print('  free bytes left in the first root sector: %d' % (DATA - last_in_first))
    if not ok:
        print('\nFIX: move a file out of the disc.xml root into <dir name="TEX">')
        print('     (or SND) and add the matching "\\\\TEX\\\\" to its load path.')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
