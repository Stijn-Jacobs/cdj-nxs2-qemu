# Watching what the player does on its medium

A question like "does the player read `exportExt.pdb` at all?" is hard to answer
from the outside: the deck shows no error, the file is on the medium, and the
screen stays empty either way. The emulators can log it instead.

Two layers log, because the medium can be attached two ways:

| `MEDIA_MODE` | what the deck reads | what a log line names |
| --- | --- | --- |
| `img` (a FAT image) | SCSI transfers on an emulated USB mass-storage device | `lba=` and `bytes=` |
| `rw` (a host directory, the default) | vvfat, which opens host files itself | the host path of the file |

A directory medium has no image, so its LBA says nothing on its own: the driver's
`open_file()` is where the decision is made, and that is what
`patch block/vvfat.c` logs. An image medium is the other way round, and applies
`hw/scsi/scsi-disk.c`: every access is a SCSI READ or WRITE, and the LBA can be
resolved against the filesystem on the image.

## Turning it on

```
CDJ_USB_TRACE=1 ./start.sh                 # to stderr
CDJ_USB_TRACE=/tmp/usb.log ./start.sh      # to a file
CDJ_USB_TRACE=/tmp/usb.log CDJ_USB_TRACE_LIMIT=5000 ./start.sh
```

Off is the default and costs one `getenv()` on the first access, then a bool
check per transfer: nothing is written and nothing is allocated. `0` means off,
so a script can pass the variable through unconditionally.

## Reading the log

```
cdj-usb open file=/tmp/media-show1/PIONEER/rekordbox/export.pdb
cdj-usb open file=/tmp/media-show1/PIONEER/rekordbox/exportExt.pdb
cdj-usb read lba=2048 bytes=512
```

`scripts/usb/usb_trace.py` turns either kind into a table:

```
$ python3 scripts/usb/usb_trace.py /tmp/usb.log
6 opens, 0 reads; files in the order the deck opened them:
          0  /tmp/media-show1/PIONEER/rekordbox/export.pdb
          0  /tmp/media-show1/PIONEER/djprofile.nxs
          0  /tmp/media-show1/PIONEER/MYSETTING.DAT
          0  /tmp/media-show1/PIONEER/DEVSETTING.DAT
          0  /tmp/media-show1/PIONEER/rekordbox/exportExt.pdb
```

For an image medium pass the image and the LBAs are resolved through its FAT:

```
$ python3 scripts/usb/usb_trace.py /tmp/usb.log extract/usbmedia3.img
4821 transfers, 2467344 bytes, 7 files touched
     524288  PIONEER/rekordbox/export.pdb
      73728  PIONEER/rekordbox/exportExt.pdb
      ...
```

## What it has already answered

The deck opens `exportExt.pdb` three times while booting a tagged stick, so it
does read the file. Pressing TAG LIST afterwards opens nothing at all: whatever
the deck does with the tag tree, it does from what it read at boot. An empty tag
list is therefore not a case of "the player never opened the file", and the next
question belongs to how the deck interprets the rows, not to the medium.
