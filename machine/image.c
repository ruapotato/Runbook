/* image.c — the installed system, modelled on NomnixOS.
 *
 * Two rules govern this table.
 *
 * 1. WIDE. Diagnosis is only a skill if there is somewhere to look. A system
 *    with six files means the player checks all six; a system with sixty means
 *    they have to reason about which ones matter. The width is the game.
 *
 * 2. REAL. Everything from /sbin/init upward is an actual program in the
 *    system's own language, executed by the VM. /etc/rc.boot is not a
 *    description of what booting does — it is what booting does. Corrupt it
 *    and the interpreter fails on the damaged line.
 *
 * Layout follows NomnixOS: /etc/inittab names what PID 1 runs, /etc/rc.boot is
 * the bootstrap rc, /etc/rc.d/rc.N are the runlevels, and the .svc files
 * under /etc/services.d are the services.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nom.h"
#include "machine.h"
#include "kernel.h"
#include "guestbin.h"

#define ROOT_UUID "8f41-2c07-a19d-5be3"

/* ------------------------------------------------------------ userland --
 * The scripts below are interpreted by /bin/rc, which is a COMPILED PROGRAM
 * running on our cpu. The binaries themselves live in guestbin.h. Both are
 * real files on the disk: a corrupted script fails in rc's parser, a
 * corrupted binary fails in the ELF loader or traps mid-execution.
 */

static const char *SRC_RCBOOT =
"# /etc/rc.boot -- the bootstrap rc, run by pid 1.\n"
"# Brings the filesystems online and enters the default runlevel.\n"
"echo rc.boot: bootstrap rc starting\n"
"need /sbin/svcinit\n"
"# /etc/fstab is the single source of truth for what gets mounted.\n"
"exec /sbin/mountall\n"
"run /etc/rc.d/rc.3\n";

static const char *SRC_RC3 =
"# /etc/rc.d/rc.3 -- multi-user runlevel.\n"
"echo rc.3: entering multi-user\n"
"exec /sbin/svcinit 3\n"
"exec /sbin/getty root\n";

static const char *SRC_RC0 =
"# /etc/rc.d/rc.0 -- halt.\n"
"echo rc.0: system halted\n";

/* ------------------------------------------------------------- packages -- */

static const Package PKG_BOOTLOADER = {
    "zbl", "2.06", "the bootloader",
    {
      { "/boot/zbl/zbl.cfg", NULL, 0644, NULL },
      { "/usr/sbin/zbl-install",  NULL, 0755, NULL },
      { "/usr/sbin/zbl-mkconfig", NULL, 0755, NULL },
      { "/usr/share/doc/zbl/README",
        "zbl 2.06 -- the bootloader.\n"
        "\n"
        "  /boot/zbl/zbl.cfg       the configuration, and the only file zbl reads\n"
        "  /usr/sbin/zbl-mkconfig  writes a configuration from THIS machine\n"
        "  /usr/sbin/zbl-install   writes the boot sector, and points the firmware\n"
        "                          at the disk it just wrote it to\n"
        "\n"
        "THE BOOT SECTOR IS NOT A FILE. No package owns it, `pkg verify` cannot see\n"
        "it, and reinstalling every package on the disk will not put it back. When\n"
        "the firmware says `no bootable device` and every file is perfect, that is\n"
        "what has happened. `zbl-install /dev/sda` is the repair, and because it\n"
        "rewrites the firmware's boot entry as well -- grub-install does the same --\n"
        "it is also the repair for a boot order still pointing at an empty optical\n"
        "drive.\n"
        "\n"
        "THE CONFIGURATION\n"
        "\n"
        "One directive per line. Leading whitespace is ignored, so the indentation\n"
        "inside an entry is convention and not syntax. Blank lines are ignored, and\n"
        "a line whose first non-blank character is # is a comment.\n"
        "\n"
        "There are SIX directives and no others:\n"
        "\n"
        "  default N     which entry to boot, counted from ZERO\n"
        "  timeout N     seconds the menu is offered for\n"
        "  entry LABEL   opens an entry block\n"
        "  kernel PATH   the kernel image\n"
        "  initrd PATH   the initial ramdisk\n"
        "  root SPEC     the root filesystem: UUID=<uuid>, or a device node\n"
        "\n"
        "A complete configuration, which is what this package ships and what\n"
        "`zbl-mkconfig` writes:\n"
        "\n"
        "  default 0\n"
        "  timeout 5\n"
        "\n"
        "  entry \"NomnixOS 11.4\"\n"
        "    kernel /boot/vmnomuz\n"
        "    initrd /boot/initrd\n"
        "    root UUID=8f41-2c07-a19d-5be3\n"
        "\n"
        "A directive and its value are separated by whitespace. `default=0` is not\n"
        "the same thing and is refused as an unrecognised directive. That is the\n"
        "loader being strict on purpose: a configuration it half understands is a\n"
        "machine that boots something nobody chose.\n"
        "\n"
        "WHERE AN ENTRY BEGINS AND ENDS. An `entry` line opens a block. The block\n"
        "runs to the next `entry` line, or to the end of the file. There is no\n"
        "closing delimiter and no nesting. Everything ABOVE the first `entry` line\n"
        "is global.\n"
        "\n"
        "Which has a consequence worth knowing before you edit this file: `kernel`,\n"
        "`initrd` and `root` are read from the CHOSEN ENTRY ONLY. A `kernel` line up\n"
        "in the global section belongs to no entry, so nothing reads it, and the\n"
        "loader says `no kernel line in configuration` with the line sitting there\n"
        "in front of you.\n"
        "\n"
        "WHICH ENTRY. `default N` counts from ZERO: `default 0` boots the first\n"
        "entry, `default 1` the second. The loader says which one it took and out of\n"
        "how many, and that line is the whole of the menu that matters:\n"
        "\n"
        "  zbl: booting entry 0 of 1\n"
        "\n"
        "With no `default` line at all, entry 0 is booted. With no `entry` line at\n"
        "all -- a hand-written config, or one a truncation cut down -- the whole file\n"
        "is read as a single entry, `default` is ignored and no `booting entry` line\n"
        "is printed. That is deliberate: a file in that state should fail on what it\n"
        "is MISSING rather than on its shape.\n"
        "\n"
        "REQUIRED AND OPTIONAL. Inside the entry that gets booted, `kernel`, `initrd`\n"
        "and `root` are all three required and none of them has a default. `default`\n"
        "is optional and means 0. `timeout` is optional; nothing on this machine can\n"
        "press a key during a boot, so the value is read, kept and never waited on.\n"
        "The label after `entry` is for you -- the loader identifies an entry by its\n"
        "NUMBER, which is why `booting entry 0 of 2` is the line that tells you what\n"
        "happened and the label is not.\n"
        "\n"
        "WHAT IT SAYS WHEN EACH ONE IS WRONG\n"
        "\n"
        "The whole file is checked for unknown directives BEFORE any of it is used,\n"
        "so a typo on the last line stops the boot even though every line above it is\n"
        "correct, and the message names the line:\n"
        "\n"
        "  zbl: zbl.cfg:5: unrecognised directive: timout\n"
        "\n"
        "Then, and all of these stop at the BOOTLOADER stage:\n"
        "\n"
        "  zbl: /boot/zbl/zbl.cfg: not found\n"
        "  zbl: /boot/zbl/zbl.cfg: not a file\n"
        "  zbl: default entry 3: there is only 1 entry in this configuration\n"
        "  zbl: no kernel line in configuration\n"
        "  zbl: no initrd line in configuration\n"
        "  zbl: no root line in configuration\n"
        "\n"
        "Past that point the loader is acting on what the file told it, the fault is\n"
        "no longer the loader's, and the stage the console reports moves on with it:\n"
        "\n"
        "  zbl: /boot/vmnomuz-6.4.9: not found                      (kernel)\n"
        "  zbl: /boot/vmnomuz -> /boot/vmnomuz-6.4.11: no such file (kernel)\n"
        "  zbl: /boot/vmnomuz: bad magic -- not a kernel image      (kernel)\n"
        "  zbl: /boot/initrd: not found                             (initrd)\n"
        "  initrd: waiting for /dev/disk/by-uuid/<uuid> ... timed out (30s),\n"
        "          no disk here carries that uuid                   (initrd)\n"
        "  initrd: waiting for /dev/sdb1 ... timed out (30s), no such device\n"
        "          on this machine                                  (initrd)\n"
        "\n"
        "Those last two are different faults and the wording is the difference: a\n"
        "uuid no disk carries, against a device node that is not on this machine.\n"
        "\n"
        "REGENERATING IT. `zbl-mkconfig` asks the disk for the uuid it ACTUALLY has,\n"
        "checks that /boot/vmnomuz and /boot/initrd both resolve, and writes one\n"
        "entry with `default 0`. It refuses to write anything at all when the kernel\n"
        "is not there, rather than pointing a fresh config at a file that is not:\n"
        "\n"
        "  zbl-mkconfig: /boot/vmnomuz is not there -- fix the kernel\n"
        "                package first, or the config will point at\n"
        "                something that does not exist\n"
        "\n"
        "`pkg reinstall zbl` also puts a working config back. The difference is where\n"
        "the answer comes from: zbl-mkconfig generates it from the machine in front\n"
        "of you and cannot be wrong about the uuid, and reinstalling writes what the\n"
        "package ships. Note that this file is NOT under /etc, so the rule that a\n"
        "plain reinstall leaves edited configuration alone does not cover it: a\n"
        "reinstall overwrites zbl.cfg, and so does zbl-mkconfig. Both write\n"
        "`timeout 5`. If somebody raised the timeout on purpose, `pkg diff` it before\n"
        "you regenerate it and put the line back afterwards.\n"
        "\n"
        "See /usr/share/doc/kernel-default/README for the images this file points at.\n",
        0644, NULL },
      { "/usr/share/doc/zbl/CHANGELOG",
        "zbl CHANGELOG -- newest first.\n"
        "\n"
        "2.06 -- current. `pkg list` says so.\n"
        "        The configuration has ENTRIES. Lines before the first `entry` are\n"
        "        global, each `entry` opens a block that runs to the next one, and\n"
        "        `default N` says which block to boot, counting from zero.\n"
        "        Before this the loader took the first `kernel`, `initrd` and `root`\n"
        "        line found anywhere in the file and ignored `default` and `entry`\n"
        "        completely -- so the menu was decoration, and the commonest real\n"
        "        bootloader mistake there is, an upgrade that appends an entry and\n"
        "        leaves the default naming the old one, could not be expressed at\n"
        "        all, let alone diagnosed.\n"
        "        The whole file is now checked for unknown directives before any of\n"
        "        it is used, and the message names the line number. A configuration\n"
        "        that is half understood is worse than one that is refused.\n"
        "\n"
        "2.05 -- The loader prints the version it read out of the kernel image's own\n"
        "        header rather than the version in the filename. Those two can\n"
        "        disagree -- a restore from backup, a downgrade, an image copied over\n"
        "        the top of another one -- and when they do, the filename is the\n"
        "        thing lying.\n"
        "\n"
        "2.04 -- `root` accepts a device node as well as UUID=. It was always legal\n"
        "        and people always wrote it; refusing it meant a hand-written config\n"
        "        failed with a parse error instead of with the sentence that is\n"
        "        actually true, which is that the device is not there.\n"
        "\n"
        "2.02 -- zbl-install writes the firmware's boot entry as well as the sector.\n"
        "        Installing a loader and not telling the firmware where it went is\n"
        "        half a job, and the half it left undone looks exactly like a dead\n"
        "        disk.\n"
        "\n"
        "2.00 -- Initial packaging for NomnixOS 11. zbl-mkconfig generates from the\n"
        "        installed kernels and the disk's own uuid.\n", 0644, NULL },
      { "/usr/share/doc/zbl/known-issues",
        "zbl -- known issues.\n"
        "\n"
        "0. FIRST: /boot/zbl/zbl.cfg READS AS CHANGED ON A HEALTHY MACHINE.\n"
        "\n"
        "   Very often, and it is not a fault. This is a configuration file and\n"
        "   people edit it -- a raised timeout with a comment above it saying why is\n"
        "   the commonest edit there is. `pkg diff /boot/zbl/zbl.cfg` shows what the\n"
        "   file says against what the package ships. Read that before you treat a\n"
        "   CHANGED here as the answer, and read it again before you regenerate the\n"
        "   file, because regenerating throws the edit away.\n"
        "\n"
        "1. `default` POINTS PAST THE END OF THE LIST.\n"
        "\n"
        "   Somebody added an entry to test something, booted it, deleted the entry\n"
        "   and left the number where it was. There is a perfectly good entry sitting\n"
        "   right there and the loader will not guess, which is what a bootloader\n"
        "   should do.\n"
        "\n"
        "     zbl: default entry 3: there is only 1 entry in this configuration\n"
        "\n"
        "   Down at the BOOTLOADER stage, so the console stops before it has named a\n"
        "   kernel or an initrd at all, and nothing in /boot is implicated. `cat\n"
        "   /boot/zbl/zbl.cfg`, count the `entry` lines, and remember they are\n"
        "   numbered from zero. One number in one line is the fix, or `zbl-mkconfig`.\n"
        "\n"
        "2. TWO ENTRIES, AND THE DEFAULT IS THE ONE THAT IS GONE.\n"
        "\n"
        "   The upgrade appended the new entry and left `default 0` naming last\n"
        "   release's, whose kernel was removed when /boot filled. Both entries are\n"
        "   well formed. The configuration contains the right answer and picks the\n"
        "   wrong one.\n"
        "\n"
        "     zbl: booting entry 0 of 2\n"
        "     zbl: /boot/vmnomuz-6.3.12: not found\n"
        "\n"
        "   `booting entry 0 of 2` is the whole diagnosis and it is easy to read past\n"
        "   on the way to the error under it. The repair is the `default` line, or\n"
        "   `zbl-mkconfig`, which writes one entry for the kernel that is here.\n"
        "\n"
        "3. THE ONLY ENTRY NAMES A KERNEL THAT IS NOT THERE.\n"
        "\n"
        "   Same message, one entry instead of two, and a different repair: there is\n"
        "   no other entry to switch to.\n"
        "\n"
        "     zbl: booting entry 0 of 1\n"
        "     zbl: /boot/vmnomuz-6.4.9: not found\n"
        "\n"
        "   `ls /boot` shows what is really there and `stat /boot/vmnomuz` is\n"
        "   perfectly happy, because the file the LOADER was told to want is not the\n"
        "   file the system installs. The symlink is not involved. `zbl-mkconfig`.\n"
        "\n"
        "4. THE ROOT IS NAMED BY A DEVICE THAT IS NOT ON THIS MACHINE.\n"
        "\n"
        "   Written by hand, the way it was written for twenty years, and then a disk\n"
        "   was added and the numbering moved underneath it. This is the reason\n"
        "   installers write uuids.\n"
        "\n"
        "     initrd: waiting for /dev/nvme0n1p2 ... timed out (30s), no such device\n"
        "             on this machine\n"
        "\n"
        "   Down at the INITRD stage, before any of userland exists, so there is no\n"
        "   prompt on that machine and nothing to type at -- the way in is the rescue\n"
        "   medium. The sister fault is a well-formed uuid that no disk here carries,\n"
        "   and the console says that in different words on purpose:\n"
        "\n"
        "     initrd: waiting for /dev/disk/by-uuid/e0e2-61e2-4d4f-afa0 ... timed out\n"
        "             (30s), no disk here carries that uuid\n"
        "\n"
        "   `blkid` says what /dev/sda1 really is. `zbl-mkconfig` writes it for you.\n"
        "   Check /etc/fstab too: it carries a uuid as well, the same mistake lives\n"
        "   in both files, and the fstab one fails much later, in mountall, with the\n"
        "   machine half up.\n"
        "\n"
        "5. THE TWO SYMLINKS IN /boot ARE THE WRONG WAY ROUND.\n"
        "\n"
        "   A rebuild script that took its arguments in the other order. /boot/vmnomuz\n"
        "   points at the initrd image and /boot/initrd points at the kernel. Both\n"
        "   links resolve to something real, both files are perfect, `ls /boot` looks\n"
        "   completely healthy, and the loader is handed an initrd where it expects a\n"
        "   kernel:\n"
        "\n"
        "     zbl: /boot/vmnomuz: bad magic -- not a kernel image\n"
        "\n"
        "   Nothing is wrong with this package or with the configuration. `stat\n"
        "   /boot/vmnomuz` and `stat /boot/initrd` is four seconds and the whole\n"
        "   answer; `pkg verify kernel-default` says REPOINTED for both and `pkg\n"
        "   reinstall kernel-default` puts them back.\n"
        "\n"
        "6. THE KERNEL IS VALID AND IS THE WRONG VERSION FOR /lib/modules.\n"
        "\n"
        "   The loader loads it, says which version it really read, and the boot dies\n"
        "   just after the root is mounted -- because the modules that kernel needs\n"
        "   live on the filesystem that has only now become readable.\n"
        "\n"
        "     zbl: loading /boot/vmnomuz (6.3.12)\n"
        "     kernel: /lib/modules holds: 6.4.11\n"
        "     kernel: /lib/modules/6.3.12: no modules for this kernel -- the image\n"
        "             and /lib/modules are out of step\n"
        "\n"
        "   Nothing here is zbl's, and the repair is not in this package. The two\n"
        "   lines together say which half is the odd one out; see\n"
        "   /usr/share/doc/kernel-default/known-issues.\n"
        "\n"
        "7. THE MACHINE HAS NOTHING TO BOOT AND EVERY FILE IS PERFECT.\n"
        "\n"
        "   `pkg verify` is clean, the disk is clean, and the console stops in the\n"
        "   FIRMWARE, above the loader, before zbl.cfg has been opened.\n"
        "\n"
        "     no bootable device -- insert boot media\n"
        "\n"
        "   is the boot sector, which is not a file and which no package owns.\n"
        "\n"
        "     zbios: /dev/sr0: no medium in the drive\n"
        "     zbios: nothing to boot -- the boot order lists only the removable drive\n"
        "\n"
        "   is the boot order: somebody installed from the optical drive, finished\n"
        "   the job, took the disc out and never put the order back, and the machine\n"
        "   has been up ever since on the strength of nobody rebooting it.\n"
        "\n"
        "   `zbl-install /dev/sda` is the repair for both, because it writes the\n"
        "   sector and the firmware's boot entry together. `rcon boot disk` from the\n"
        "   service processor sets the order the other way. Note that zbl-install\n"
        "   refuses to run when there is no /boot/zbl/zbl.cfg for it to install a\n"
        "   loader for -- write the configuration first.\n", 0644, NULL },
    }, 6
};

static const Package PKG_KERNEL = {
    "kernel-default", "6.4.11", "the kernel and its initrd",
    {
      { "/boot/vmnomuz-6.4.11", "\x7fKRNL 6.4.11 rv64\n", 0644, NULL },
      { "/boot/vmnomuz", NULL, 0777, "/boot/vmnomuz-6.4.11" },
      { "/boot/initrd-6.4.11",
        "\x7fINITRD 6.4.11\n"
        "module virtio_blk\n"
        "module ext4\n"
        "module dm_mod\n", 0644, NULL },
      { "/boot/initrd", NULL, 0777, "/boot/initrd-6.4.11" },
      { "/usr/bin/mkinitrd", NULL, 0755, NULL },
      /* THE MODULE DIRECTORY IS OWNED, and it has to be, for the reason
       * /var/log and /run already are: `open` with O_CREAT makes a file and
       * never the path above it, so once a cleanup or a half-finished upgrade
       * has taken /lib/modules/6.4.11 away, `pkg reinstall kernel-default`
       * cannot put a single .ko back -- it has nowhere to write them. The
       * solve ladder scored exactly that unfixable. Listed BEFORE the modules
       * so the manifest restores the directory first. */
      { "/lib/modules/6.4.11", NULL, 0755, NULL, true },
      { "/lib/modules/6.4.11/virtio_blk.ko", "\x7fMOD virtio_blk\n", 0644, NULL },
      { "/lib/modules/6.4.11/ext4.ko",       "\x7fMOD ext4\n",       0644, NULL },
      { "/lib/modules/6.4.11/dm_mod.ko",     "\x7fMOD dm_mod\n",     0644, NULL },
      { "/lib/modules/6.4.11/e1000.ko",      "\x7fMOD e1000\n",      0644, NULL },
      { "/lib/modules/6.4.11/loop.ko",       "\x7fMOD loop\n",       0644, NULL },
      { "/usr/share/doc/kernel-default/README",
        "kernel-default 6.4.11 -- the kernel and its initrd.\n"
        "\n"
        "  /boot/vmnomuz-6.4.11    the kernel image\n"
        "  /boot/vmnomuz           A SYMLINK to it\n"
        "  /boot/initrd-6.4.11     the initial ramdisk\n"
        "  /boot/initrd            A SYMLINK to it\n"
        "  /lib/modules/6.4.11/    the modules: virtio_blk, ext4, dm_mod, e1000, loop\n"
        "  /usr/bin/mkinitrd       rebuilds the initrd from what is in /lib/modules\n"
        "\n"
        "THE TWO SYMLINKS ARE THE MOST IMPORTANT FACT IN THIS FILE. The bootloader\n"
        "config names /boot/vmnomuz; the package installs /boot/vmnomuz-6.4.11 and\n"
        "points the link at it. So there are two things that can go wrong and they\n"
        "look nothing alike:\n"
        "\n"
        "  ls /boot           shows you the LINK. It looks healthy either way.\n"
        "  stat /boot/vmnomuz FOLLOWS it, and tells you the truth.\n"
        "\n"
        "`pkg verify kernel-default` records the hash of a symlink's TARGET, so it\n"
        "reports MISSING (symlink) and REPOINTED as distinct things.\n"
        "\n"
        "WHAT THE INITRD IS FOR: it carries the drivers needed to reach the root\n"
        "filesystem before the root filesystem is available. This machine's disk is\n"
        "virtio_blk and its filesystem is ext4, so an initrd without those two cannot\n"
        "mount the root however perfect the rest of the machine is.\n"
        "\n"
        "mkinitrd BUILDS FROM /lib/modules. It cannot invent a module that is not\n"
        "there. That is why a deleted module is two repairs in order -- reinstall the\n"
        "package, THEN rebuild -- and a merely wrong initrd is one.\n"
        "\n"
        "THREE THINGS HAVE TO AGREE, and the whole family of upgrade failures is what\n"
        "happens when they do not:\n"
        "\n"
        "  /boot/vmnomuz-<v>   the kernel image\n"
        "  /lib/modules/<v>/   the modules built for that kernel\n"
        "  /boot/initrd-<v>    the ramdisk built out of those modules\n"
        "\n"
        "THE VERSION IS NOT THE FILENAME. Every image carries its own in its first\n"
        "line -- `cat /boot/vmnomuz-6.4.11` says `KRNL 6.4.11 rv64`, and\n"
        "`cat /boot/initrd-6.4.11` says `INITRD 6.4.11` above its module list -- and\n"
        "that is the one the loader reads and prints:\n"
        "\n"
        "  zbl: loading /boot/vmnomuz (6.4.11)\n"
        "\n"
        "A filename is a label somebody typed; a header is what the image really is.\n"
        "When the two disagree it is the filename that is lying, which is the whole\n"
        "reason that line exists. To ask a running machine which kernel it actually\n"
        "loaded, `uname` or `cat /proc/version` -- both report what the loader put\n"
        "there, not what is installed. On a machine that never got that far, the\n"
        "loader's own line is what you have: `dmesg -f loading`.\n"
        "\n"
        "WHAT IS CHECKED AND IN WHAT ORDER, because a stage is half the diagnosis:\n"
        "\n"
        "  zbl: loading /boot/vmnomuz (6.4.11)   the image is found, and read\n"
        "  kernel 6.4.11 booting                 the initrd is found, and read\n"
        "  initrd: mounted ... on /              root mounted; /lib/modules is ON it\n"
        "  then /lib/modules/<the version the image says> must exist and hold\n"
        "  at least one file, and then the initrd's version must equal the\n"
        "  kernel's.\n"
        "\n"
        "/lib/modules is checked BEFORE the initrd's version, deliberately: when the\n"
        "kernel image is itself the odd one out, everything on the disk disagrees\n"
        "with it at once, and the console should name the single thing that is wrong\n"
        "rather than blame the initrd for being right.\n"
        "\n"
        "WHAT mkinitrd REALLY BUILDS, because deciding what to do next depends on it.\n"
        "It reads /lib/modules/6.4.11, writes one `module <name>` line per .ko it\n"
        "finds there, and refuses if virtio_blk, ext4 or dm_mod is not among them.\n"
        "That is the whole of it. THERE IS NO SHELL IN THE INITRD and no userland of\n"
        "any kind: an initrd on this machine is a list of drivers. So a boot that\n"
        "stops in the initrd has no prompt on it and nothing to type at, and the way\n"
        "in is the rescue medium, not a rescue shell that does not exist. mkinitrd\n"
        "also writes the VERSIONED file only and never touches the /boot/initrd\n"
        "symlink, so a link pointing at the wrong thing is not something rebuilding\n"
        "can repair.\n", 0644, NULL },
      { "/usr/share/doc/kernel-default/CHANGELOG",
        "kernel-default CHANGELOG -- newest first.\n"
        "\n"
        "6.4.11 -- current. `pkg list` says so and so does /lib/modules.\n"
        "\n"
        "6.4.9  -- superseded. IF THE BOOTLOADER CONFIG STILL NAMES THIS VERSION,\n"
        "          that is the fault: an upgrade that half finished, or a cleanup\n"
        "          that removed the old image after the config was written. The\n"
        "          loader says `/boot/vmnomuz-6.4.9: not found`, `ls /boot` shows\n"
        "          what is really there, and `stat /boot/vmnomuz` is perfectly happy\n"
        "          -- because the file the LOADER wants is not the file the system\n"
        "          installs. `zbl-mkconfig` writes a config for the machine in front\n"
        "          of you; reinstalling the zbl package writes the config for the\n"
        "          machine it was built for.\n"
        "\n"
        "6.4.4  -- the initrd records which modules it contains, so the loader can\n"
        "          say what an image DOES carry as well as what it lacks. \"No driver\n"
        "          for the root device\" was the same sentence for an empty initrd and\n"
        "          for one built on a machine with different disks, and those are not\n"
        "          the same problem.\n"
        "\n"
        "6.3.x  -- older series. No modules for this series remain installed;\n"
        "          /lib/modules has one directory and it matches this kernel. When it\n"
        "          does not match, drivers refuse to load, and that is the commonest\n"
        "          real upgrade failure there is.\n", 0644, NULL },
      { "/usr/share/doc/kernel-default/known-issues",
        "kernel-default -- known issues.\n"
        "\n"
        "1. THE VERSIONED IMAGE IS GONE AND THE LISTING LOOKS FINE.\n"
        "\n"
        "   Something tidied /boot. /boot/vmnomuz is a symlink and the symlink was\n"
        "   not touched, so `ls /boot` is perfectly healthy and the loader says the\n"
        "   kernel is missing.\n"
        "\n"
        "     ls /boot            the link\n"
        "     stat /boot/vmnomuz  the truth\n"
        "     pkg verify kernel-default\n"
        "     pkg reinstall kernel-default\n"
        "\n"
        "2. THE IMAGE IS TRUNCATED OR ITS MAGIC IS WRONG.\n"
        "\n"
        "   Half a file, usually because /boot filled during the write. The write\n"
        "   \"succeeded\". The two cases read differently and it is worth knowing\n"
        "   which you have: `pkg verify` says TRUNCATED, and how many bytes are\n"
        "   gone off the end, when what is there is the shipped image and then\n"
        "   nothing -- that is the write that ran out of room. It says CHANGED when\n"
        "   the bytes that ARE there are wrong, which is a different accident.\n"
        "   Reinstall -- and check `df` first, or you will write half a file again.\n"
        "\n"
        "3. THE INITRD HAS NO DRIVER FOR THIS DISK.\n"
        "\n"
        "   Two different faults with one symptom, and the loader distinguishes them\n"
        "   by saying what the image DOES carry.\n"
        "\n"
        "   a. The module is missing from /lib/modules as well. mkinitrd cannot\n"
        "      invent it. TWO repairs, in order: `pkg reinstall kernel-default` then\n"
        "      `mkinitrd`.\n"
        "   b. Every module is present in /lib/modules and the initrd is simply the\n"
        "      wrong one -- a complete, valid image full of drivers for somebody\n"
        "      else's hardware, which is what a clone from a different box produces.\n"
        "      ONE repair: `mkinitrd`.\n"
        "\n"
        "   `ls /lib/modules/6.4.11` is what tells you which of the two you have.\n"
        "\n"
        "4. /lib/modules DOES NOT MATCH THE KERNEL THAT WAS LOADED.\n"
        "\n"
        "   The directory is named for one version and the image is another. Nothing\n"
        "   loads, and the boot dies just AFTER the root was mounted, because\n"
        "   /lib/modules is on the filesystem that had only then become readable.\n"
        "   The console prints both halves, and which half is the odd one out decides\n"
        "   the repair:\n"
        "\n"
        "     zbl: loading /boot/vmnomuz (6.4.11)\n"
        "     kernel: /lib/modules holds: 6.3.12\n"
        "     kernel: /lib/modules/6.4.11: no modules for this kernel -- the image\n"
        "             and /lib/modules are out of step\n"
        "\n"
        "   Here the kernel is the one this machine is meant to run and the modules\n"
        "   are last release's -- an upgrade where the new kernel went in and the\n"
        "   modules did not, or a cleanup that took the wrong directory. `pkg verify\n"
        "   kernel-default` reports /lib/modules/6.4.11 MISSING along with every .ko\n"
        "   in it, and `pkg reinstall kernel-default` puts them back. This package\n"
        "   OWNS that directory, which is what makes it repairable at all: creating a\n"
        "   file never creates the path above it.\n"
        "\n"
        "5. THE IMAGE IS A VALID KERNEL OF THE WRONG VERSION.\n"
        "\n"
        "   The mirror image of 4, and it reads almost the same. A restore from a\n"
        "   backup, or an image copied over the top of another one: the file is where\n"
        "   it belongs, it has the right name, it loads, and it is last release's\n"
        "   kernel.\n"
        "\n"
        "     zbl: loading /boot/vmnomuz (6.3.12)\n"
        "     kernel: /lib/modules holds: 6.4.11\n"
        "\n"
        "   Read the two lines together. In 4 the modules are the odd one out; here\n"
        "   the IMAGE is, and `ls /boot` cannot see it because the filename still\n"
        "   says 6.4.11 -- only the loader's version, or `cat` on the image, does.\n"
        "   `pkg verify kernel-default` says CHANGED for /boot/vmnomuz-6.4.11, which\n"
        "   is the confirmation, and reinstalling is the repair. If it got as far as\n"
        "   a prompt, `uname` agrees with the loader -- it reads /proc/version, so\n"
        "   it reports the image that loaded and not the one on the shelf.\n"
        "\n"
        "6. THE INITRD WAS BUILT FOR ANOTHER KERNEL.\n"
        "\n"
        "   Not empty, not corrupt, not foreign hardware: a complete image full of\n"
        "   exactly the right modules for the kernel that was running when somebody\n"
        "   rebuilt it, which is not the kernel installed now.\n"
        "\n"
        "     initrd: /boot/initrd was built for 6.3.12, and this kernel is 6.4.11 --\n"
        "             the initrd is the odd one out\n"
        "\n"
        "   The console names the odd one out because it can: /lib/modules was\n"
        "   checked first and agreed with the kernel. `mkinitrd` builds one for the\n"
        "   kernel that is here and that is the entire repair -- no package is\n"
        "   damaged and reinstalling achieves nothing.\n"
        "\n"
        "7. THE TWO SYMLINKS ARE THE WRONG WAY ROUND.\n"
        "\n"
        "   A rebuild script that took its arguments in the other order. /boot/vmnomuz\n"
        "   resolves to the initrd image and /boot/initrd to the kernel. Both links\n"
        "   resolve, both files are perfect, `ls /boot` is completely healthy, and\n"
        "   the loader is handed an initrd where it expects a kernel:\n"
        "\n"
        "     zbl: /boot/vmnomuz: bad magic -- not a kernel image\n"
        "\n"
        "   `stat /boot/vmnomuz` and `stat /boot/initrd` is four seconds and the\n"
        "   whole answer. `pkg verify kernel-default` says REPOINTED for both --\n"
        "   distinct from MISSING (symlink), which is the link itself gone, and from\n"
        "   MISSING on the versioned file, which is the link left dangling. Reinstall\n"
        "   this package; mkinitrd cannot help, because it never writes the links.\n",
        0644, NULL },
    }, 14
};

/* The userland that actually runs. */
static const Package PKG_SYSINIT = {
    "sysinit", "254", "pid 1, the rc scripts and the runlevels",
    {
      { "/usr/lib/sysinit/init", NULL, 0755, NULL },   /* GUEST_INIT */
      { "/sbin/init", NULL, 0777, "/usr/lib/sysinit/init" },
      { "/sbin/svcinit", NULL, 0755, NULL },           /* GUEST_SVCINIT */
      { "/sbin/login",   NULL, 0755, NULL },           /* GUEST_LOGIN   */
      { "/sbin/getty",   NULL, 0755, NULL },           /* GUEST_GETTY   */
      { "/sbin/mountall", NULL, 0755, NULL },          /* GUEST_MOUNTALL */
      { "/etc/inittab",
        "# /etc/inittab -- the last non-comment line is run by /sbin/init.\n"
        "/bin/rc /etc/rc.boot\n", 0644, NULL },
      { "/etc/rc.boot",   NULL, 0755, NULL },          /* SRC_RCBOOT */
      { "/etc/rc.d/rc.3", NULL, 0755, NULL },          /* SRC_RC3 */
      { "/etc/rc.d/rc.0", NULL, 0755, NULL },          /* SRC_RC0 */
      { "/etc/rc.conf", "3\n", 0644, NULL },
      { "/usr/share/doc/sysinit/README",
        "sysinit 254 -- pid 1, the rc scripts, the runlevels and the login.\n"
        "\n"
        "  /usr/lib/sysinit/init   the real program\n"
        "  /sbin/init              a symlink to it\n"
        "  /etc/inittab            the ONE file pid 1 reads\n"
        "  /etc/rc.boot            the bootstrap rc\n"
        "  /etc/rc.d/rc.3          the multi-user runlevel\n"
        "  /etc/rc.d/rc.0          halt\n"
        "  /etc/rc.conf            the default runlevel\n"
        "  /sbin/svcinit           starts the units in /etc/services.d\n"
        "  /sbin/mountall          reads /etc/fstab, and decides ro or rw for /\n"
        "  /sbin/getty, /sbin/login\n"
        "\n"
        "THE ORDER, because the stage a machine stops at is half the diagnosis:\n"
        "\n"
        "  init      reads /etc/inittab. Its last non-comment line is a command.\n"
        "  rc        /bin/rc runs /etc/rc.boot.\n"
        "  mountall  /etc/fstab is the SINGLE source of truth for what is mounted,\n"
        "            including the remount of the root itself as ro or rw.\n"
        "  runlevel  rc.boot runs /etc/rc.d/rc.3.\n"
        "  services  /sbin/svcinit reads /etc/services.d and starts units in\n"
        "            dependency order.\n"
        "  login     /sbin/getty checks the account is in /etc/passwd and that its\n"
        "            shell exists and is executable, then offers a prompt.\n"
        "\n"
        "/bin/rc KNOWS FIVE VERBS: echo, mount, run, exec, need. It stops at the\n"
        "FIRST failure, deliberately -- a boot that carries on past a step that did\n"
        "not work is a boot that fails later, somewhere unrelated, for reasons that\n"
        "make no sense. That also means rc is a poor language for a checklist, which\n"
        "is why the half-written one in ~nomowner/bin is all comments.\n"
        "\n"
        "A boot that dies with almost nothing on the console died in init, and init\n"
        "reads one file.\n", 0644, NULL },
      { "/usr/share/doc/sysinit/known-issues",
        "sysinit -- known issues.\n"
        "\n"
        "1. /etc/inittab NAMES A SCRIPT THAT IS NOT THERE.\n"
        "\n"
        "   The file is two lines long and one of them is now a path that does not\n"
        "   exist -- somebody testing single-user mode, or reorganising the runlevel\n"
        "   scripts. The machine stops before any of userland has run and the console\n"
        "   has almost nothing on it, which is itself the diagnosis.\n"
        "\n"
        "2. A `need` LINE IN /etc/rc.boot FOR SOMETHING THAT IS NOT INSTALLED.\n"
        "\n"
        "   A vendor package dropped it in, or somebody tidied away the thing it\n"
        "   named. rc stops at the first failure, so the machine dies at a line that\n"
        "   has nothing to do with booting. The repair is to take the line out, not\n"
        "   to install anything.\n"
        "\n"
        "   Note that an extra `echo` in rc.boot is somebody being helpful and is\n"
        "   completely harmless. Same file, same kind of edit, opposite verdict --\n"
        "   read the verb.\n"
        "\n"
        "3. A UNIT IN THE WRONG RUNLEVEL.\n"
        "\n"
        "   Nothing failed and nothing was tried. The unit is present, correct,\n"
        "   enabled and healthy, and it belongs to runlevel 5 on a machine that boots\n"
        "   to 3. `enabled: yes` is in the file and is the line everybody reads. The\n"
        "   word \"runlevel\" appears only on the console. The mirror image is rc.3\n"
        "   entering runlevel 5, where half the service set does not belong.\n"
        "\n"
        "4. TWO UNITS ORDERED AFTER EACH OTHER.\n"
        "\n"
        "   Neither is broken, neither will ever start, and each unit read on its own\n"
        "   is completely reasonable. Reading ONE file tells you nothing. This is the\n"
        "   fault that punishes checking a file instead of checking a set.\n"
        "\n"
        "5. ORDERED AFTER SOMETHING THAT IS NOT INSTALLED, OR IS DISABLED.\n"
        "\n"
        "   svcinit says which, and the difference is twenty minutes: \"waiting for\n"
        "   network\", \"waiting for network -- and no unit by that name is installed\",\n"
        "   and \"waiting for network -- which is disabled\" are three sentences. Read\n"
        "   the console; `svc` will show the dependents DEAD with no reason at all,\n"
        "   and `pkg verify` will point at the wrong service.\n"
        "\n"
        "6. A STRAY .svc NO PACKAGE OWNS.\n"
        "\n"
        "   `pkg verify` is CLEAN and the boot still stops, because verify compares\n"
        "   what is installed against the manifests and a file no manifest mentions\n"
        "   cannot differ from anything. `pkg owns` it; \"nothing owns it\" is a real\n"
        "   answer; delete it.\n", 0644, NULL },
    }, 13
};

static const Package PKG_BASE = {
    "filesystem", "84.87", "the base layout and system identity",
    {
      { "/etc/fstab",    NULL, 0644, NULL },
      { "/etc/hostname", NULL, 0644, NULL },
      { "/etc/os-release",
        "NAME=\"NomnixOS\"\nVERSION=\"11.4\"\nID=nomnix\n"
        "PRETTY_NAME=\"NomnixOS 11.4\"\n", 0644, NULL },
      { "/etc/lsb-release",
        "DISTRIB_ID=NomnixOS\nDISTRIB_RELEASE=11.4\n", 0644, NULL },
      { "/etc/issue", "NomnixOS 11.4\n", 0644, NULL },
      { "/etc/motd",  "Welcome to NomnixOS.\n", 0644, NULL },
      /* THE SHELLS AN ACCOUNT MAY LOG IN WITH, and getty checks it. This
       * said `/bin/nomsh`, a program that has not existed for two releases,
       * and nothing read the file -- so it was decoration that would have
       * locked every account out the day anything started honouring it. */
      { "/etc/shells", "/bin/sh\n/bin/false\n", 0644, NULL },
      { "/etc/profile", "# login shell profile\nPATH=/bin:/usr/bin:/sbin\n", 0644, NULL },
          { "/run", NULL, 0755, NULL, true },
      { "/tmp", NULL, 0777, NULL, true },
      { "/var/cache", NULL, 0755, NULL, true },
          { "/usr/share/doc/README",
        "/usr/share/doc -- one directory per package, named exactly as `pkg list`\n"
        "names the package.\n"
        "\n"
        "  README         what the package is for, and which files on this machine\n"
        "                 belong to it\n"
        "  CHANGELOG      the versions, newest first. The top entry is the version\n"
        "                 `pkg list` reports, and if it is not, one of the two is\n"
        "                 lying and it is worth knowing which.\n"
        "  known-issues   the ways this package really goes wrong HERE, what each one\n"
        "                 looks like in `pkg verify`, and what the repair is. These\n"
        "                 are not hypothetical. Every one of them has happened on a\n"
        "                 machine like this one.\n"
        "\n"
        "The documentation is shipped BY the package it documents, which is the whole\n"
        "reason it is trustworthy: `pkg owns /usr/share/doc/httpd/README` answers\n"
        "`httpd`, `pkg verify httpd` hashes it along with the binary and the config,\n"
        "and a doc file that has been edited shows up as CHANGED like anything else.\n"
        "Nothing here is a second source of truth bolted on beside the machine.\n"
        "\n"
        "Not every package has a directory. A package with nothing interesting to say\n"
        "does not get made to say something, which is the only way the ones that do\n"
        "have anything stay worth reading.\n"
        "\n"
        "  ls /usr/share/doc\n"
        "  ls /usr/share/doc/httpd\n"
        "  cat /usr/share/doc/libc/known-issues\n"
        "  grep -n verify /usr/share/doc/libc/known-issues\n"
        "\n"
        "A glob matches names in ONE directory, so `/usr/share/doc/*/README` is\n"
        "not a thing you can write here: the shell splits at the last slash and\n"
        "asks the kernel to list a directory called `*`. Glob the last part or\n"
        "use `find /usr/share/doc -name README`.\n"
        "\n"
        "`man` is the other half of this: `man` on its own lists the pages, and those\n"
        "are about COMMANDS. These are about PACKAGES. When something is broken you\n"
        "usually know which package before you know which command.\n", 0644, NULL },
    }, 12
};

static const Package PKG_USERS = {
    "shadow", "4.13", "accounts",
    {
      { "/etc/passwd",
        "root:x:0:0:root:/root:/bin/sh\n"
        "daemon:x:1:1:daemon:/:/bin/false\n"
        "nomowner:x:1000:1000:host owner:/home/nomowner:/bin/sh\n", 0644, NULL },
      { "/etc/group", "root:x:0:\ndaemon:x:1:\nnomowner:x:1000:\n", 0644, NULL },
      { "/etc/shadow", "root:!:19000:0:99999:7:::\n", 0600, NULL },
      { "/etc/login.defs", "UID_MIN 1000\nUID_MAX 60000\n", 0644, NULL },
      { "/usr/share/doc/shadow/README",
        "shadow 4.13 -- accounts.\n"
        "\n"
        "  /etc/passwd      name:x:uid:gid:comment:home:shell   -- SEVEN fields\n"
        "  /etc/group\n"
        "  /etc/shadow      the password, mode 0600\n"
        "  /etc/login.defs\n"
        "\n"
        "/sbin/getty VALIDATES THE ACCOUNT before it hands the machine over. The\n"
        "entry has to be in /etc/passwd, and the login shell named in the last field\n"
        "has to exist and be executable. That is why a machine can boot perfectly,\n"
        "with every service running, and have no way in at all -- which is a\n"
        "different problem from a machine that will not start, and is why login is\n"
        "its own boot stage here.\n"
        "\n"
        "Count the colons. Six of them, seven fields. Almost everything that goes\n"
        "wrong with this file is arithmetic.\n", 0644, NULL },
      { "/usr/share/doc/shadow/known-issues",
        "shadow -- known issues. Every one of these boots the machine perfectly.\n"
        "\n"
        "1. THE LOGIN SHELL IS WRONG.\n"
        "\n"
        "   A shell that used to exist, one that never did, a rename that was going\n"
        "   to be temporary, or the field left empty. getty says so by name. `ls -l`\n"
        "   the path it names.\n"
        "\n"
        "2. root IS NOT IN /etc/passwd AT ALL.\n"
        "\n"
        "   Half a user migration. Everything runs; nobody can log in.\n"
        "\n"
        "3. /etc/passwd AND /etc/shadow ARE OUT OF STEP.\n"
        "\n"
        "   root is in passwd and has no line in shadow. THE PASSWORD LIVES IN THE\n"
        "   OTHER FILE, and /etc/passwd -- where everybody looks first -- is perfect.\n"
        "   `cat /etc/shadow` is the whole diagnosis and mode 0600 is why people\n"
        "   forget it is there.\n"
        "\n"
        "4. ONE EXTRA COLON.\n"
        "\n"
        "   The line still parses. Right name, right uid, right home. Every field\n"
        "   after the typo has shifted one to the left, so the login shell is now the\n"
        "   home directory, and the machine says -- quite correctly -- that root's\n"
        "   login shell /root is not a program. A sentence that makes no sense at all\n"
        "   until you count the colons.\n"
        "\n"
        "5. A SERVICE ACCOUNT SOMEBODY ADDED.\n"
        "\n"
        "   `pkg verify shadow` says CHANGED on /etc/passwd and it is not the fault.\n"
        "   Somebody added an account on purpose. `pkg diff` it: a diff that reads\n"
        "   like a decision is not a fault, and `pkg reinstall --force` here deletes\n"
        "   a real account to fix a problem that is somewhere else.\n", 0644, NULL },
    }, 6
};

static const Package PKG_NET = {
    "netcfg", "11.6", "network configuration and daemon",
    {
      { "/usr/sbin/netd", NULL, 0755, NULL },
      { "/etc/services.d/net.svc",
        "# /etc/services.d/net.svc\n"
        "name: net\n"
        "critical: yes\n"
        "exec: /usr/sbin/netd\n"
        "description: network interfaces\n"
        "after: syslog\n"
        "restart: on-failure\n"
        "enabled: yes\n"
        "runlevel: 3\n", 0644, NULL },
      { "/etc/net/interfaces", "iface eth0\n  address dhcp\n", 0644, NULL },
      { "/etc/hosts",
        "127.0.0.1       localhost nominal.local\n"
        "10.0.2.20       wiki.nomnix.org wiki\n"
        "10.0.2.30       support.internal support\n"
        "10.0.2.44       bofh.nomnix.org bofh\n", 0644, NULL },
      { "/etc/resolv.conf", "nameserver 10.0.2.3\nsearch nomnix.org\n", 0644, NULL },
      { "/etc/host.conf", "order hosts,bind\n", 0644, NULL },
      { "/etc/networks", "default 0.0.0.0\n", 0644, NULL },
      { "/etc/protocols", "ip 0 IP\ntcp 6 TCP\nudp 17 UDP\n", 0644, NULL },
      { "/etc/services", "ssh 22/tcp\nhttp 80/tcp\n", 0644, NULL },
      /* NOBODY DOCUMENTED THE FORMAT OF THE FILE EVERY ADDRESS COMES OUT OF.
       *
       * `man ip` tells you to edit /etc/net/interfaces and reload; nothing
       * anywhere said what to write in it. That was survivable while every
       * machine had one card and one stanza, and it stopped being when the
       * file grew a stanza per interface so a floor server's tagged
       * subinterfaces would come back after a power cut. The format changed
       * and its documentation did not exist to change.
       *
       * Every line below was read off read_ifaces() and cfg_field() in
       * core/netsite.c. */
      { "/usr/share/doc/netcfg/README",
        "netcfg 11.6 -- the addresses, and the daemon that applies them.\n"
        "\n"
        "  /usr/sbin/netd          reads the file and makes the cards agree\n"
        "  /etc/net/interfaces     what the cards are supposed to be\n"
        "  svc reload net          re-read it after editing\n"
        "\n"
        "THE FILE IS A LIST OF STANZAS, ONE PER INTERFACE. A stanza opens\n"
        "with `iface` and a card name, and the lines under it belong to that\n"
        "card until the next `iface`:\n"
        "\n"
        "  iface eth0\n"
        "    address 10.0.1.10\n"
        "    netmask 24\n"
        "    gateway 10.0.1.1\n"
        "  iface eth1.13\n"
        "    address 10.13.0.10\n"
        "    netmask 24\n"
        "\n"
        "  iface <name>     eth0, eth1, or a tagged subinterface eth1.13.\n"
        "                   NAMING ONE IS WHAT CREATES IT: a subinterface\n"
        "                   this file mentions exists after a reboot, which\n"
        "                   is how a floor server's vlans and the dhcp pools\n"
        "                   riding on them survive a power cut.\n"
        "  address <ip>     a dotted address, or the word `dhcp` to ask for\n"
        "                   one. `dhcp` really sends a DISCOVER; if nothing\n"
        "                   answers, the card comes up with no address and\n"
        "                   `ip addr` says so.\n"
        "  netmask <n>      24, or the dotted 255.255.255.0. Both are read.\n"
        "                   A stanza with no netmask gets the site default.\n"
        "  gateway <ip>     the DEFAULT ROUTE, which belongs to the machine\n"
        "                   and not to a card -- so it is read once and the\n"
        "                   FIRST one in the file wins, wherever it sits.\n"
        "  # ...            a comment. Blank lines are ignored.\n"
        "\n"
        "The same card named twice is one card: the second stanza adds to the\n"
        "first rather than replacing it.\n"
        "\n"
        "WHAT SERVICES A BOX RUNS IS A DIFFERENT FILE -- /etc/net/services,\n"
        "one line each: `dhcpd <first> <count> <bits> <gw> <dns>`, `dnsd`,\n"
        "and `record <name> <ip>` for a name that server answers for. A\n"
        "`record` with no `dnsd` before it starts nothing, which is the\n"
        "honest reading of a zone with no server behind it.\n"
        "\n"
        "The editor is ed(1). `ed /etc/net/interfaces ,n` numbers the lines.\n"
        "See also ip(8), which SHOWS what the cards really hold and cannot\n"
        "change it, and netstat(8).\n", 0644, NULL },
    }, 10
};

static const Package PKG_SYSLOG = {
    "syslog", "2.4", "system logging",
    {
      { "/usr/sbin/syslogd", NULL, 0755, NULL },
      { "/etc/services.d/syslog.svc",
        "# /etc/services.d/syslog.svc\n"
        "name: syslog\n"
        "critical: yes\n"
        "exec: /usr/sbin/syslogd\n"
        "description: system logging\n"
        "after: udev\n"
        "restart: on-failure\n"
        "enabled: yes\n"
        "runlevel: 3\n", 0644, NULL },
      { "/etc/syslog.conf", "*.info /var/log/messages\n", 0644, NULL },
          { "/var/log", NULL, 0755, NULL, true },
      { "/usr/share/doc/syslog/README",
        "syslog 2.4 -- system logging.\n"
        "\n"
        "  /usr/sbin/syslogd            the daemon\n"
        "  /etc/syslog.conf             where messages go\n"
        "  /var/log                     A DIRECTORY THIS PACKAGE OWNS\n"
        "  /etc/services.d/syslog.svc   critical: yes, after udev\n"
        "\n"
        "/var/log/messages is NOT owned by any package and never will be. `pkg owns\n"
        "/var/log/messages` answers \"nothing\", and that is a fact worth being able to\n"
        "discover: a log that matched a hash would be a log nothing had written to.\n"
        "The DIRECTORY is owned, so a deleted or chmod'd /var/log is something\n"
        "`pkg verify syslog` can see and `pkg reinstall syslog` can repair. The\n"
        "contents are yours.\n"
        "\n"
        "syslogd appends its own banner at every boot, so the newest line in\n"
        "/var/log/messages is always this machine's own and everything above it is\n"
        "history. /var/log/messages.1 is what logrotate moved aside, and on a machine\n"
        "that has been running a while it is the more interesting of the two.\n"
        "\n"
        "  dmesg                  this boot, from the kernel\n"
        "  dmesg -1               the boot before this one\n"
        "  tail /var/log/messages the end of the log, which is where the news is\n"
        "  grep <name> /var/log/messages.1   what happened last time\n", 0644, NULL },
      { "/usr/share/doc/syslog/known-issues",
        "syslog -- known issues, and the first one is a warning about this package.\n"
        "\n"
        "1. SYSLOGD IS ALMOST NEVER THE PROBLEM.\n"
        "\n"
        "   It is critical, it starts early, and it is the first thing on the machine\n"
        "   that tries to WRITE. So when the disk is full, or the root is mounted\n"
        "   read-only, or /var/log has had its mode changed, syslogd is the first\n"
        "   service to fail and every instinct you have says to go and look at\n"
        "   syslogd.\n"
        "\n"
        "   The first thing to fail is never the interesting thing. It is just the\n"
        "   first thing that tried.\n"
        "\n"
        "     df          is there room\n"
        "     df -i       ...or is it inodes, which is a different question\n"
        "     mount       is the root ro\n"
        "     ls -ld /var/log\n"
        "\n"
        "   Two hours went into syslogd on this machine in March. The answer was\n"
        "   `df`, and `df` is free.\n"
        "\n"
        "2. THE LOG ATE THE DISK.\n"
        "\n"
        "   One enormous file. `wc /var/log/messages` finds it immediately, `tail` it\n"
        "   before you delete it, and `rm` it -- syslogd writes a fresh one at the\n"
        "   next start. Nothing is corrupt and `pkg verify` will hand you a clean\n"
        "   bill of health while there is nowhere to put the next byte.\n"
        "\n"
        "   Compare: a package CACHE that ate the disk is four hundred ordinary\n"
        "   files, none of them remarkable, and no amount of looking at files will\n"
        "   show it to you. `find /var -type f` is what shows you a directory.\n"
        "\n"
        "3. /var/log IS GONE, OR IS NOT WRITABLE.\n"
        "\n"
        "   Because this package owns the directory, verify says so plainly --\n"
        "   `missing`, or `MODE` with the mode it shipped. `pkg reinstall syslog`\n"
        "   restores the directory. Note that this is a directory a package owns; the\n"
        "   mode of a directory NOBODY owns is invisible to verify entirely, and then\n"
        "   `ls -ld` is the only tool that will tell you.\n"
        "\n"
        "4. SYSLOGD DOES NOT PUBLISH A STATE FILE, AND ALMOST EVERYTHING ELSE DOES.\n"
        "\n"
        "   `ls /run` shows udevd, netd, sshd, nomde, crond, ntpd, httpd, nft and\n"
        "   auditd each writing down what they actually loaded, and no syslogd\n"
        "   among them. So the trick that catches every other stale config --\n"
        "   compare /run/<name>.state against the file on disk -- has nothing to\n"
        "   compare here. An edit to /etc/syslog.conf that nothing reloaded has to\n"
        "   be caught by noticing that the machine is not doing what the file says.\n"
        "   `ps` for the pid, then `kill -HUP` on it.\n", 0644, NULL },
    }, 6
};

static const Package PKG_UDEV = {
    "udev", "254", "device node management",
    {
      { "/usr/sbin/udevd", NULL, 0755, NULL },
      { "/etc/services.d/udev.svc",
        "# /etc/services.d/udev.svc\n"
        "name: udev\n"
        "critical: yes\n"
        "exec: /usr/sbin/udevd\n"
        "description: device manager\n"
        "restart: on-failure\n"
        "enabled: yes\n"
        "runlevel: 3\n", 0644, NULL },
      /* The rule that NAMES the network device. udev is what decides an
       * interface is called eth0, and netd configures whatever udev named --
       * so these two files have to agree, and a rename in one of them is a
       * real and thoroughly confusing fault. Before this the rules file was
       * read by udevd and consulted by nothing, which is exactly the
       * "file nothing reads" this project is not supposed to have. */
      { "/etc/udev/rules.d/50-default.rules",
        "SUBSYSTEM==\"block\", MODE=\"0660\"\n"
        "SUBSYSTEM==\"net\", NAME=\"eth0\"\n", 0644, NULL },
    }, 3
};

/* The previous administrator. Everything here is flavour EXCEPT that some of
 * it is true and useful -- the notes describe faults this machine really has
 * had, in the voice of someone who was tired. A player who reads them is
 * better at the job than one who does not, which is the only kind of easter
 * egg worth hiding. */
/* THE PERSON WHO HAD THIS JOB BEFORE YOU.
 *
 * NOMINAL had a home directory you could reconstruct a sysadmin from -- a
 * diary, a handover, a vendor thread that never answers, an automation
 * project abandoned for a reason its own README states. It was one of the
 * best things in that game and it is about somebody else's job, so this is
 * the same idea rewritten for this one.
 *
 * IT IS ALSO THE TUTORIAL, and that is why it is worth the words. The naming
 * convention is in notes.txt because a player who goes looking finds it
 * there; the ordering rules are in notes.txt because that is where somebody
 * doing this job would have written them; and bin/README lists the three
 * things wrong with the half-finished script, which are precisely the three
 * things Act II is about. A player who reads the home directory out of
 * curiosity comes back knowing the job.
 *
 * Every technical sentence in here is true of THIS world and was checked by
 * running it. A player who reads a machine and is lied to never reads
 * anything again.
 *
 * And the diary is the game's own argument, in somebody's voice: the day
 * they wrote the loop was the best day they had, and they should have had it
 * in week one. That is §3, and nobody has to be told it.
 */
static const Package PKG_HOME = {
    "pvane-home", "1.0", "the previous administrator's home directory",
    {
      { "/root/Desktop/read-me-first.txt",
        "If you are reading this you have my job.\n"
        "\n"
        "Sorry about the state of it. The short version is that Harbrook was ninety\n"
        "people when I started and it is not any more, and I never got ahead of it.\n"
        "\n"
        "WHERE THINGS ARE\n"
        "\n"
        "  The queue is the queue. Everything that needs doing turns up there and\n"
        "  nothing closes until it is actually done -- you cannot mark something\n"
        "  finished, it just goes when the checks pass. That took me a fortnight to\n"
        "  stop resenting and it is the only reason this place still works.\n"
        "\n"
        "  Three boxes do the work. The directory (accounts, groups, who may use\n"
        "  what), the mail server, and the file server. They all have web pages and\n"
        "  they all have an API, which is the same thing underneath. I did not know\n"
        "  that for two months.\n"
        "\n"
        "  My notes are in /home/pvane. The naming convention is in notes.txt and it\n"
        "  matters more than it looks like it does.\n"
        "\n"
        "WHAT I WOULD DO DIFFERENTLY\n"
        "\n"
        "  Automate the onboarding on your first week, not your fourth month. I have\n"
        "  left what I got as far as in /home/pvane/bin -- it is half a script and it\n"
        "  has a note attached saying exactly why I stopped, which in hindsight was\n"
        "  not a good reason.\n"
        "\n"
        "  There is a record button on the panel. I found it in February.\n"
        "\n"
        "-- P. Vane\n"
        "\n", 0644, NULL },
      { "/home/pvane/notes.txt",
        "Things I keep having to look up.\n"
        "\n"
        "THE NAMING CONVENTION\n"
        "  First initial, then the family name, all lowercase, nothing else. Alma\n"
        "  Barrow is abarrow.\n"
        "\n"
        "  If it is taken you add a number and start at 2 -- abarrow2. You do NOT\n"
        "  reuse a login because somebody left. It is not free. Their mail is still\n"
        "  in it and the file server still has their folder, and I have seen what\n"
        "  happens when a new starter opens somebody else's inbox on their first\n"
        "  morning. It happened here. Do not do it.\n"
        "\n"
        "  There is no service that hands you a login. Work it out, then ASK the\n"
        "  directory whether it is free, then create it. In that order.\n"
        "\n"
        "GROUPS\n"
        "  Every department has one: dept-engineering, dept-sales, and so on. People\n"
        "  get groups, groups get applications. Nobody gets an application directly.\n"
        "  I set it up that way on purpose -- when finance asked for everybody in the\n"
        "  team to get the ledger it was one line, not forty.\n"
        "\n"
        "  You can make your own group kinds. They are just names.\n"
        "\n"
        "THE ORDER THINGS HAVE TO HAPPEN IN\n"
        "  Account, then group. An alias needs its mailbox. A share grant needs the\n"
        "  share to exist. If you do it the other way round it does not half-work, it\n"
        "  fails -- which is better, and it did not feel like it the first time.\n"
        "\n"
        "THE FILE SERVER\n"
        "  Halcyon says 200 to everything. Everything. Including the failures. If you\n"
        "  script against it you have to read the body, and the body is XML because of\n"
        "  course it is. See vendor-halcyon.txt if you want to be annoyed.\n"
        "\n", 0644, NULL },
      { "/home/pvane/TODO",
        "- finish the onboarding script (bin/README says why I stopped, it is a\n"
        "  bad reason, ignore it and finish it)\n"
        "\n"
        "- work out why the mail server slows down around 500 mailboxes. I think it\n"
        "  just does. Buy another one before it matters, not after.\n"
        "\n"
        "- ask about a second file server BEFORE the quarter ends this time\n"
        "\n"
        "- the leavers list. Nobody tells me. People just stop being here and their\n"
        "  account stays until somebody notices, and the somebody is always me and\n"
        "  it is always six weeks late.\n"
        "\n"
        "- learn to say no to \"can you just\"\n"
        "\n"
        "- holiday\n"
        "\n", 0644, NULL },
      { "/home/pvane/Documents/diary.txt",
        "Not a diary really. Somewhere to put the reasons.\n"
        "\n"
        "--- MARCH ---\n"
        "\n"
        "2nd. Ninety-one people. Four starters this week, all of them on Monday\n"
        "because HR does everything on Monday. Took me most of the morning. That is\n"
        "fine, mornings are for this.\n"
        "\n"
        "11th. Found out the web forms are just the API. There is a terminal on this\n"
        "machine and everything the buttons do, it does. I have been clicking through\n"
        "six forms a person for four months.\n"
        "\n"
        "I am not going to write down how that felt.\n"
        "\n"
        "12th. `rb ticket.list open`. Twelve of them. It fits on one screen and I have\n"
        "been finding that out one form at a time.\n"
        "\n"
        "--- APRIL ---\n"
        "\n"
        "3rd. Hundred and twenty. Wrote a loop today. A real one, in the shell:\n"
        "\n"
        "    for t in $(rb ticket.list open | grep TCK); do echo $t; done\n"
        "\n"
        "It does not do anything yet. I sat and looked at it for a bit anyway.\n"
        "\n"
        "19th. Hundred and forty. The loop does something now. It made four accounts\n"
        "in about a second and I checked all four by hand afterwards because I did not\n"
        "believe it.\n"
        "\n"
        "--- MAY ---\n"
        "\n"
        "8th. Two hundred and ten. The script has stopped being right. Two people this\n"
        "week had the same login and it made one account and quietly moved on, and I\n"
        "did not find out until the second one could not log in. It does not ask. It\n"
        "just assumes.\n"
        "\n"
        "I know how to fix it. I have not.\n"
        "\n"
        "21st. Two eighty. Halcyon told me a home folder was created. It was not. It\n"
        "said 200. It says 200 to everything.\n"
        "\n"
        "--- JUNE ---\n"
        "\n"
        "4th. Three hundred and ten. I am not writing these up any more, there is not\n"
        "time. The script does the easy ones and I do the rest and the rest is\n"
        "growing faster than the easy ones.\n"
        "\n"
        "17th. Interview on Thursday.\n"
        "\n"
        "--- JULY ---\n"
        "\n"
        "1st. Told them. Two months' notice, which is generous of me.\n"
        "\n"
        "The thing I would say to whoever comes next: the day I wrote the loop was\n"
        "the best day I had here. I should have had it in week one. Everything after\n"
        "that was me not fixing a script I already knew was wrong, because there was\n"
        "never a morning free to do it, because the script was not fixed.\n"
        "\n", 0644, NULL },
      { "/home/pvane/bin/README",
        "What is in here.\n"
        "\n"
        "  onboard-half.py   the onboarding script, unfinished\n"
        "\n"
        "WHY IT IS UNFINISHED\n"
        "\n"
        "I stopped because it was wrong in three ways and I could only see two of\n"
        "them, and fixing two out of three felt worse than fixing none.\n"
        "\n"
        "The three:\n"
        "\n"
        "  1. It does not check whether the login is free. It works out the name and\n"
        "     creates the account. When two people collide it makes one account and\n"
        "     carries on, and you find out weeks later.\n"
        "\n"
        "  2. It does not look at what came back. Halcyon answers 200 to failures,\n"
        "     so a run that reports forty successes might be thirty-four.\n"
        "\n"
        "  3. It does not retry. Calls fail sometimes -- not often, but every day at\n"
        "     this size, and one failed call leaves somebody half-made.\n"
        "\n"
        "Number 1 is ten lines. Ask the directory before you create; if it says the\n"
        "login is there, put a 2 on the end and ask again.\n"
        "\n"
        "Number 2 is one line per call, if you make the call in a variable and look\n"
        "at it.\n"
        "\n"
        "Number 3 is a loop around a call.\n"
        "\n"
        "That is the whole list. I wrote it out in February meaning to do it that\n"
        "weekend. If you are reading this, do it -- it is an afternoon, and it is the\n"
        "afternoon that gives you every other afternoon back.\n"
        "\n"
        "-- pv\n"
        "\n", 0644, NULL },
      { "/home/pvane/bin/onboard-half.py",
        "# onboard-half.py -- as far as I got.\n"
        "#\n"
        "# Read bin/README before you trust this. It is wrong in three known ways and\n"
        "# they are all listed there with what to do about them.\n"
        "#\n"
        "#   py /home/pvane/bin/onboard-half.py\n"
        "\n"
        "def convention(given, family):\n"
        "    return lower(sub(given, 0, 1)) + lower(family)\n"
        "\n"
        "for line in lines(api(\"ticket.list open 40\")):\n"
        "    if find(line, \"user.onboard\") < 0:\n"
        "        continue\n"
        "    t = json(line)\n"
        "    u = json(api(\"user.get \" + t[\"ref\"]))\n"
        "    dept = u[\"dept\"]\n"
        "\n"
        "    # WRONG (1): never asks whether this login is free.\n"
        "    login = convention(u[\"given\"], u[\"family\"])\n"
        "\n"
        "    # WRONG (2): does not look at any of these answers.\n"
        "    # WRONG (3): does not retry any of them.\n"
        "    api(\"api.call directory_01 create_account login=\" + login + \" user_ref=\" + t[\"ref\"] + \" display_name=\\\"\" + u[\"given\"] + \" \" + u[\"family\"] + \"\\\" dept=\" + dept + \" status=active\")\n"
        "    api(\"api.call directory_01 add_member login=\" + login + \" group=dept-\" + dept)\n"
        "    api(\"api.call mail_01 create_mailbox login=\" + login + \" address=\" + login + \"@harbrook.example quota_mb=2048 status=active\")\n"
        "    api(\"api.call fileserver_01 create_home login=\" + login + \" path=/home/\" + login + \" quota_mb=8192\")\n"
        "    api(\"api.call fileserver_01 grant_share login=\" + login + \" share=share-\" + dept + \" access=rw\")\n"
        "\n"
        "    print(\"did\", t[\"id\"], login)\n"
        "\n", 0755, NULL },
      { "/home/pvane/Documents/vendor-halcyon.txt",
        "Thread with Halcyon support, kept because I want the receipts.\n"
        "\n"
        "---\n"
        "ME, 14 March\n"
        "  Hello. create_home on FS/9 returns HTTP 200 with an error inside the body.\n"
        "  Is that intended? It makes it impossible to tell success from failure\n"
        "  without parsing every response.\n"
        "\n"
        "HALCYON, 16 March\n"
        "  Thank you for contacting Halcyon Data Works. Could you confirm the firmware\n"
        "  version?\n"
        "\n"
        "ME, 16 March\n"
        "  4.1.2. The version is not the issue -- the API returns 200 on failure.\n"
        "\n"
        "HALCYON, 19 March\n"
        "  Thank you. Our records show FS/9 is operating within specification.\n"
        "\n"
        "ME, 19 March\n"
        "  Which part of the specification says a failed write returns 200?\n"
        "\n"
        "HALCYON, 24 March\n"
        "  The status code indicates that the request was received and processed.\n"
        "  Please refer to the response body for the outcome of the operation.\n"
        "\n"
        "ME, 24 March\n"
        "  \"Processed\" is doing a lot of work in that sentence.\n"
        "\n"
        "HALCYON, 2 April\n"
        "  We have logged your feedback for consideration in a future release.\n"
        "---\n"
        "\n"
        "For consideration in a future release.\n"
        "\n"
        "If you are pricing a replacement: Veridian is roughly twice the money and\n"
        "answers honestly, and I have done the arithmetic on that more than once. Two\n"
        "hours a month of not being lied to pays for it in a quarter.\n"
        "\n", 0644, NULL },
      { "/home/pvane/.sh_history",
        "rb ticket.list open\n"
        "rb ticket.list open | grep TCK\n"
        "rb ticket.get TCK-04412\n"
        "rb user.get u_00311\n"
        "rb api.call directory_01 get_account login=jkeel\n"
        "rb api.call directory_01 get_account login=jkeel2\n"
        "rb api.call directory_01 create_account login=jkeel2 user_ref=u_00311 display_name=\"Jarek Keel\" dept=finance status=active\n"
        "rb ticket.check TCK-04412\n"
        "for t in $(rb ticket.list open | grep TCK); do echo $t; done\n"
        "py bin/onboard-half.py\n"
        "rb ticket.stats\n"
        "rb api.call fileserver_01 get_home login=jkeel2\n"
        "rb api.call fileserver_01 create_home login=jkeel2 path=/home/jkeel2 quota_mb=8192\n"
        "rb api.call fileserver_01 get_home login=jkeel2\n"
        "rb ticket.stats\n"
        "\n", 0644, NULL },
      { "/home/pvane/Documents/handover.txt",
        "Handover.\n"
        "\n"
        "THE JOB\n"
        "  Everything that needs doing arrives in the queue. Read it, do it, and the\n"
        "  game -- sorry, the system -- decides when it is done. There is no button.\n"
        "\n"
        "  Roughly six things per new starter, across three boxes. Twelve minutes if\n"
        "  you are quick and nothing goes wrong.\n"
        "\n"
        "THE PEOPLE\n"
        "  About four hundred at the moment. It was ninety in October. It doubles\n"
        "  roughly every three weeks at the current rate and nobody upstairs thinks\n"
        "  that is remarkable.\n"
        "\n"
        "THE BOXES\n"
        "  directory_01   accounts, groups, application access. Veridian. Good kit.\n"
        "  mail_01        mailboxes and aliases. Veridian.\n"
        "  fileserver_01  home folders and department shares. Halcyon. Read\n"
        "                 vendor-halcyon.txt before you write anything against it.\n"
        "\n"
        "  Each holds about 600 people comfortably. Past that they get slow, and slow\n"
        "  is not a warning light, it is your afternoon. Buy the next one early.\n"
        "\n"
        "WHAT I NEVER GOT TO\n"
        "  See TODO. The important one is the script. bin/README says what is wrong\n"
        "  with it and what to do; it is honestly an afternoon's work and I never had\n"
        "  an afternoon.\n"
        "\n"
        "Good luck. It is a good job when you are ahead of it.\n"
        "\n"
        "-- pv\n"
        "\n", 0644, NULL },
      { "/home/pvane/.profile",
        "# pvane's shell profile.\n"
        "#\n"
        "# Not much here. The shell on this machine is small and I stopped fighting it.\n"
        "\n"
        "# The two I typed the most. `rb` is the API, and everything the web forms do\n"
        "# goes through it -- which took me four months to notice, so: it is written\n"
        "# down here where you will trip over it.\n"
        "#\n"
        "#   rb ticket.list open\n"
        "#   rb ticket.stats\n"
        "#\n"
        "# There is no alias support, so those are just notes. Sorry.\n"
        "\n", 0644, NULL },
    }, 10
};

static const Package PKG_SSH = {
    "openssh", "9.4", "remote login",
    {
      { "/usr/sbin/sshd", NULL, 0755, NULL },
      { "/etc/services.d/sshd.svc",
        "# /etc/services.d/sshd.svc\n"
        "name: sshd\n"
        "exec: /usr/sbin/sshd\n"
        "description: remote login\n"
        "after: net\n"
        "restart: on-failure\n"
        "enabled: yes\n"
        "runlevel: 3\n", 0644, NULL },
      { "/etc/ssh/sshd_config", "Port 22\nPermitRootLogin no\n", 0644, NULL },
    }, 3
};

static const Package PKG_HAMDE = {
    "nomde", "3.1", "the desktop",
    {
      { "/usr/bin/nomde", NULL, 0755, NULL },
      { "/etc/services.d/nomde.svc",
        "# /etc/services.d/nomde.svc\n"
        "name: nomde\n"
        "exec: /usr/bin/nomde\n"
        "description: the display server\n"
        /* RUNLEVEL 3 AND 5, so the graphical stack exists on every machine
         * and can therefore BREAK on every machine. A desktop that only runs
         * on a box you never see is not debuggable, and David's whole point
         * was being able to debug a broken graphical session the way you
         * would a broken X11 one. */
        "after: net\n"
        "restart: on-failure\n"
        "enabled: yes\n"
        "runlevel: 3 5\n", 0644, NULL },
      { "/etc/nomde/panel.conf", "position=bottom\nheight=28\n", 0644, NULL },
      { "/etc/nomde/desktop.icons", "Terminal\nFiles\n", 0644, NULL },
      /* THE APP REGISTRY, and it is FILES.
       *
       * David: "tie the desktop into the OS at a fairly deep level, so you
       * could start any of the graphical applications from the command line
       * ... similar to debugging a broken X11 session."
       *
       * So the desktop does not know what applications exist. It reads these,
       * the way every real desktop reads .desktop files. Delete one and the
       * icon goes; corrupt one and it goes; and both are diagnosable with
       * `ls /usr/share/applications` and `cat` like anything else. The
       * graphical stack becomes a thing you can break and repair rather than
       * a painted-on menu. */
      { "/usr/share/applications/terminal.desktop",
        "[Desktop Entry]\n"
        "Name=Terminal\n"
        "Exec=terminal\n"
        "Icon=term\n"
        "Comment=A shell on this machine\n", 0644, NULL },
      { "/usr/share/applications/files.desktop",
        "[Desktop Entry]\n"
        "Name=Files\n"
        "Exec=files\n"
        "Icon=files\n"
        "Comment=Browse this machine\n", 0644, NULL },
      { "/usr/share/applications/notes.desktop",
        "[Desktop Entry]\n"
        "Name=Notes\n"
        "Exec=notes\n"
        "Icon=notes\n"
        "Comment=/root/notes.txt, in a window\n", 0644, NULL },
      { "/usr/share/applications/logview.desktop",
        "[Desktop Entry]\n"
        "Name=Log Viewer\n"
        "Exec=logview\n"
        "Icon=log\n"
        "Comment=What the machine said while booting\n", 0644, NULL },
      { "/usr/share/applications/manual.desktop",
        "[Desktop Entry]\n"
        "Name=Manual\n"
        "Exec=manual\n"
        "Icon=manual\n"
        "Comment=How this machine works\n", 0644, NULL },
      { "/usr/share/applications/browser.desktop",
        "[Desktop Entry]\n"
        "Name=Browser\n"
        "Exec=browser\n"
        "Icon=browser\n"
        "Comment=The intranet\n", 0644, NULL },
      { "/usr/share/applications/g2048.desktop",
        "[Desktop Entry]\n"
        "Name=2048\n"
        "Exec=g2048\n"
        "Icon=tiles\n"
        "Comment=Slide the tiles\n", 0644, NULL },
      { "/usr/share/applications/gflappy.desktop",
        "[Desktop Entry]\n"
        "Name=Flappy\n"
        "Exec=gflappy\n"
        "Icon=flappy\n"
        "Comment=Do not hit the pipes\n", 0644, NULL },
      { "/usr/share/applications/gworms.desktop",
        "[Desktop Entry]\n"
        "Name=Worms\n"
        "Exec=gworms\n"
        "Icon=worms\n"
        "Comment=Two worms, one hill\n", 0644, NULL },
      { "/usr/share/applications/gsnake.desktop",
        "[Desktop Entry]\n"
        "Name=Snake\n"
        "Exec=gsnake\n"
        "Icon=snake\n"
        "Comment=Do not eat yourself\n", 0644, NULL },
      { "/usr/share/applications/gmines.desktop",
        "[Desktop Entry]\n"
        "Name=Minesweeper\n"
        "Exec=gmines\n"
        "Icon=mines\n"
        "Comment=The first click is always safe\n", 0644, NULL },
      { "/usr/share/applications/gblocks.desktop",
        "[Desktop Entry]\n"
        "Name=Blocks\n"
        "Exec=gblocks\n"
        "Icon=blocks\n"
        "Comment=Seven shapes, one well\n", 0644, NULL },
      { "/usr/share/applications/gsolitaire.desktop",
        "[Desktop Entry]\n"
        "Name=Solitaire\n"
        "Exec=gsolitaire\n"
        "Icon=cards\n"
        "Comment=Klondike, draw one or three\n", 0644, NULL },
      { "/usr/share/applications/gliquid.desktop",
        "[Desktop Entry]\n"
        "Name=Liquid War\n"
        "Exec=gliquid\n"
        "Icon=liquid\n"
        "Comment=Three hundred fighters follow your cursor\n", 0644, NULL },
      { "/usr/share/applications/calc.desktop",
        "[Desktop Entry]\n"
        "Name=Calculator\n"
        "Exec=calc\n"
        "Icon=calc\n"
        "Comment=Arithmetic, with precedence\n", 0644, NULL },
      /* THE THREE THAT LOOK AT A MACHINE.
       *
       * These are graphical front ends to ps, svc, df, and pkg -- they run
       * the same commands you would type and show you what came back. That
       * is deliberate: nothing here can tell you something the shell would
       * not, so a player who prefers the terminal loses no information, and
       * a player who prefers the window is never shown a comforting lie. */
      { "/usr/share/applications/sysmon.desktop",
        "[Desktop Entry]\n"
        "Name=System Monitor\n"
        "Exec=sysmon\n"
        "Icon=sysmon\n"
        "Comment=Processes, services and storage\n", 0644, NULL },
      { "/usr/share/applications/pkgman.desktop",
        "[Desktop Entry]\n"
        "Name=Package Manager\n"
        "Exec=pkgman\n"
        "Icon=pkg\n"
        "Comment=What is installed, and what has changed\n", 0644, NULL },
      { "/usr/share/applications/svcman.desktop",
        "[Desktop Entry]\n"
        "Name=Service Manager\n"
        "Exec=svcman\n"
        "Icon=svc\n"
        "Comment=Start, stop and read what died\n", 0644, NULL },
      /* The music player reads /usr/share/sounds the way this desktop reads
       * /usr/share/applications: the playlist is a directory listing, so
       * `rm /usr/share/sounds/hamnix-demo.wav` empties the playlist and
       * `pkg reinstall nomnix-sounds` fills it again. A player with its
       * tracks compiled into the window would be a second source of truth
       * about a filesystem the player can already read with `ls`. */
      { "/usr/share/applications/music.desktop",
        "[Desktop Entry]\n"
        "Name=Music\n"
        "Exec=music\n"
        "Icon=music\n"
        "Comment=Play what is in /usr/share/sounds\n", 0644, NULL },
      { "/usr/share/applications/gsand.desktop",
        "[Desktop Entry]\n"
        "Name=Falling Sand\n"
        "Exec=gsand\n"
        "Icon=sand\n"
        "Comment=Twelve materials and one density table\n", 0644, NULL },
      { "/usr/share/applications/gsetris.desktop",
        "[Desktop Entry]\n"
        "Name=Sand Tetris\n"
        "Exec=gsetris\n"
        "Icon=sandtris\n"
        "Comment=Pieces dissolve; bridge a colour wall to wall\n", 0644, NULL },
      { "/usr/share/applications/clock.desktop",
        "[Desktop Entry]\n"
        "Name=Clock\n"
        "Exec=clock\n"
        "Icon=clock\n"
        "Comment=Time, calendar, timer and stopwatch\n", 0644, NULL },
      { "/usr/share/applications/imgview.desktop",
        "[Desktop Entry]\n"
        "Name=Image Viewer\n"
        "Exec=imgview\n"
        "Icon=imgview\n"
        "Comment=Look at what is picture-shaped\n", 0644, NULL },
      { "/usr/share/applications/archman.desktop",
        "[Desktop Entry]\n"
        "Name=Archive Manager\n"
        "Exec=archman\n"
        "Icon=archman\n"
        "Comment=Browse a package as a tree of files\n", 0644, NULL },
      { "/usr/share/applications/duview.desktop",
        "[Desktop Entry]\n"
        "Name=Disk Usage\n"
        "Exec=duview\n"
        "Icon=duview\n"
        "Comment=Where the disk went\n", 0644, NULL },
      { "/usr/share/applications/charmap.desktop",
        "[Desktop Entry]\n"
        "Name=Character Map\n"
        "Exec=charmap\n"
        "Icon=charmap\n"
        "Comment=Every character the font can draw\n", 0644, NULL },
      { "/usr/share/applications/search.desktop",
        "[Desktop Entry]\n"
        "Name=Search\n"
        "Exec=search\n"
        "Icon=search\n"
        "Comment=A front end to find\n", 0644, NULL },
      /* Where the display server takes requests. A file, because a file can
       * be looked at: `cat /run/nomde/requests` shows what was asked for,
       * which is the debuggability the socket version would not have. */
      { "/etc/nomde/nomde.conf",
        "# the display server\n"
        "socket = /run/nomde/requests\n"
        "applications = /usr/share/applications\n", 0644, NULL },
      { "/usr/bin/open", NULL, 0755, NULL },
      /* THE DISPLAY SERVER OWNS ITS OWN DIRECTORIES.
       *
       * /run/nomde was created at install and owned by nobody, so when the
       * deleted-directory fault took /run out, `pkg reinstall` could put back
       * /run -- which base owns -- and not /run/nomde, and nomde never came
       * up again. One seed of sixty went unfixable that way. A package owns
       * the directories it needs, or it cannot be repaired. */
      { "/run/nomde", NULL, 0755, NULL, true },
      { "/usr/share/applications", NULL, 0755, NULL, true },
    }, 35
};

/* THE SOUNDS ARE FILES, AND A PACKAGE OWNS THEM.
 *
 * The desktop plays the real samples out of its own resources -- a WAV is a
 * megabyte of PCM and the guest disk is modelled byte for byte, so putting
 * the audio itself on it would cost the player's RAM for nothing anybody can
 * hear. What lives here is the ENTRY: a name, a mode, an owner and a size,
 * which is everything `ls`, `pkg verify` and `pkg reinstall` need. The music
 * player lists this directory and shows what it finds, so deleting a track
 * removes it from the playlist and reinstalling this package brings it back.
 * That is the same bargain /usr/share/applications already makes with the
 * launcher, and it is what stops the playlist being a hardcoded array that
 * disagrees with the disk. */
static const Package PKG_SOUNDS = {
    "nomnix-sounds", "1.2", "the system sounds",
    {
      { "/usr/share/sounds", NULL, 0755, NULL, true },
      { "/usr/share/sounds/boot-jingle.wav",
        "RIFF WAVE 44100Hz 16bit stereo -- the startup jingle\n", 0644, NULL },
      { "/usr/share/sounds/hamnix-demo.wav",
        "RIFF WAVE 44100Hz 16bit stereo -- Hamnix music demo\n", 0644, NULL },
    }, 3
};

static const Package PKG_SHELL = {
    "nomsh", "1.13", "the shell and the base tools",
    {
      { "/bin/rc",    NULL, 0755, NULL },
      { "/bin/sh",    NULL, 0755, NULL },
      { "/bin/ls",    NULL, 0755, NULL },
      { "/bin/cat",   NULL, 0755, NULL },
      { "/bin/ps",    NULL, 0755, NULL },
      { "/bin/ns",    NULL, 0755, NULL },
      { "/bin/stat",  NULL, 0755, NULL },
      { "/bin/chmod", NULL, 0755, NULL },
      { "/bin/mount", NULL, 0755, NULL },
      { "/bin/umount", NULL, 0755, NULL },
      { "/bin/chroot", NULL, 0755, NULL },
      /* Essential here specifically: when the disk's own libc is wrong,
       * nothing on that disk runs, so the only ldd you can use is this one --
       * pointed at the broken binary through /mnt. */
      { "/usr/bin/ldd", NULL, 0755, NULL },
      /* Essential on the live medium: the whole point is reading the log of a
       * boot that failed, from a system that did not. */
      { "/bin/dmesg", NULL, 0755, NULL },
      { "/sbin/fsck", NULL, 0755, NULL },
      { "/sbin/blkid", NULL, 0755, NULL },
      { "/bin/kill", NULL, 0755, NULL },
      { "/usr/bin/svc", NULL, 0755, NULL },
      { "/usr/bin/pkg", NULL, 0755, NULL },
      { "/usr/bin/links", NULL, 0755, NULL },
      { "/bin/cp", NULL, 0755, NULL },
      { "/bin/mv", NULL, 0755, NULL },
      { "/bin/rm", NULL, 0755, NULL },
      { "/bin/touch", NULL, 0755, NULL },
      { "/bin/grep", NULL, 0755, NULL },
      { "/bin/sed", NULL, 0755, NULL },
      /* THE EDITOR. Three manual pages, the shell's own `help` and two of
       * the previous administrator's notes end with "edit the file", and for
       * a long time nothing on this machine could. `sed -i` changes a line
       * that is already there and `echo >>` adds one at the end; neither can
       * insert a line in the middle, delete the third of four, or show you
       * what you are about to change. A playtester tried vi, ed, edit, nano
       * and write and got `command not found` from all five. */
      /* THE COMPANY'S API, ON THE COMPANY'S WORKSTATION.
       *
       * The one program RUNBOOK adds to NOMINAL's userland, and the reason
       * the emulated machine is here at all (handoff decision 13): with it,
       * every verb the desktop's buttons send is one shell command away, so a
       * script on this box can do the job. Without it the machine is a very
       * elaborate text editor. See guest/rb.c. */
      { "/bin/rb", NULL, 0755, NULL },
      /* THE LANGUAGE. A Python subset -- indentation, if/elif/else, while,
       * for/in, def, integers, strings, lists and dicts -- lexed, compiled to
       * bytecode and executed BY A PROGRAM ON THIS DISK, on this CPU.
       * Decision 14 asked for Python because the audience knows it; decision
       * 13 asked for it to run here. See guest/py.c. */
      { "/bin/py", NULL, 0755, NULL },
      /* SCRIPTS THAT WORK, ON THE DISK, FROM THE FIRST MORNING.
       *
       * Handoff §15 calls M4 the hypothesis: does the relief of the first
       * script land? A player who has to invent the whole idea of automation
       * from a blank prompt mostly does not get there, and the macro recorder
       * (§16.2, still to build) is the on-ramp for the ones who never will.
       *
       * These are the other half of that ramp and they cost nothing: a
       * working script you can read, run, and then change. The comments in
       * onboard.py name the three things it does WRONG on purpose -- no
       * retry, no verification, no exceptions -- because those three are Act
       * II, and a player who finds them by reading is a player who has
       * already understood what the act is about. */
      { "/root/examples/README", NULL, 0644, NULL },
      { "/root/examples/watch.sh", NULL, 0755, NULL },
      { "/root/examples/selftest.py", NULL, 0644, NULL },
      { "/root/examples/selftest.sh", NULL, 0755, NULL },
      { "/root/examples/gunner.py", NULL, 0644, NULL },
      { "/root/examples/firewatch.py", NULL, 0644, NULL },
      { "/bin/ed", NULL, 0755, NULL },
      { "/bin/echo", NULL, 0755, NULL },
      { "/bin/wc", NULL, 0755, NULL },
      { "/bin/head", NULL, 0755, NULL },
      /* Every one of these was a reflex a playtester reached for and did not
       * get. `tail` matters most: /var/log/messages is half a megabyte, head
       * existed and tail did not, so the only end of the log the player could
       * read was the wrong one. `du` is what you type the instant df says
       * 100%. `mkdir` was documented in the protocol and absent from the
       * machine, which meant a deleted /var/log could be diagnosed and not
       * repaired. */
      { "/bin/tail", NULL, 0755, NULL },
      { "/bin/du", NULL, 0755, NULL },
      { "/bin/mkdir", NULL, 0755, NULL },
      /* `seq` is here rather than in the joke package because it is the only
       * way to say "do that four hundred times" on a shell that has `for` and
       * no arithmetic -- which is what it takes to fill a filesystem's inode
       * table by hand and watch `df` and `df -i` disagree with each other.
       * `rev` is here because it is two hundred bytes and it is the shortest
       * proof that a pipeline really carries bytes: rev | rev is identity. */
      { "/bin/seq", NULL, 0755, NULL },
      { "/bin/rev", NULL, 0755, NULL },
      { "/bin/uname", NULL, 0755, NULL },
      { "/bin/whoami", NULL, 0755, NULL },
      { "/bin/df", NULL, 0755, NULL },
      { "/bin/false", "#!false\n", 0755, NULL },
      { "/bin/true",  "#!true\n",  0755, NULL },
      /* The remote console. It lives on the technician's workstation, and
       * on the customer's machine too -- every NomnixOS install has it,
       * because every one of them might be the machine you are calling from
       * when the next ticket comes in. */
      { "/usr/bin/rcon", NULL, 0755, NULL },
      /* Both of these exist because the model kept reaching for them and a
       * playtester kept wanting them. When the thing everyone expects is
       * missing, the answer is to build it, not to explain its absence. */
      { "/usr/bin/find", NULL, 0755, NULL },
      { "/bin/netstat",  NULL, 0755, NULL },
      /* netstat says what the machine BELIEVES about the network. ping is
       * the only program on it that makes the machine TRY, and it is the
       * first thing anybody reaches for -- the management line beside the
       * rack had a ping verb, the operating system did not, and the shell is
       * where a sysadmin types it first. */
      { "/bin/ping",     NULL, 0755, NULL },
      /* THE INSTRUMENTS A NETWORK ENGINEER REACHES FOR, on the box they are
       * standing at. A playtester with a working stack under them put it
       * exactly: "inside the box I have one instrument, netstat, and it
       * cannot test reachability -- for a networking game that is
       * backwards." Every one of these reads the running stack: `ip` the
       * addresses and the table the kernel really looks in, `arp` the
       * neighbours that really answered and which card they answered on,
       * `traceroute` real probes and the real ICMP that came back,
       * `tcpdump` the frames that really crossed the card, `ss` the sockets
       * netstat already knows in the shape people type today. */
      { "/bin/ip",       NULL, 0755, NULL },
      { "/bin/arp",      NULL, 0755, NULL },
      { "/bin/traceroute", NULL, 0755, NULL },
      { "/bin/ss",       NULL, 0755, NULL },
      /* THE ONE INSTRUMENT ON THIS LIST THAT READS THE PAST. Every other
       * program here takes a reading now: the sockets now, the neighbours
       * now, the card's counters now. A call is over by the time anybody
       * sits down to ask why it was bad -- the busy period ended hours ago
       * and the streams were hung up with it -- so a live reading of one is
       * an empty screen on a desk whose day was ruined. `voice` reads the
       * record of the calls this machine has FINISHED, which the stack keeps
       * on the node beside the interface counters and for the same reason. */
      { "/bin/voice",    NULL, 0755, NULL },
      /* tcpdump lives in sbin, as it does everywhere, because reading other
       * people's frames off a card has always been root's business. */
      { "/usr/sbin/tcpdump", NULL, 0755, NULL },
      /* `init 6` worked and `reboot` did not. Nobody types `init 6` first. */
      { "/sbin/reboot",   NULL, 0755, NULL },
      { "/sbin/halt",     NULL, 0755, NULL },
      { "/sbin/poweroff", NULL, 0755, NULL },
      /* `init 0` rebooted the machine because nothing implemented it. On a
       * real system /sbin/init IS pid 1 and ALSO acts as telinit when a user
       * runs it with a runlevel, so that is what init.c does now. This is
       * only the second name for it. */
      { "/sbin/telinit", NULL, 0755, NULL },
      { "/usr/share/doc/nomsh/README",
        "nomsh 1.13 -- the shell and the base tools.\n"
        "\n"
        "/bin/sh is the interactive shell. /bin/rc is a different program with a\n"
        "different job: rc runs SCRIPT FILES during the boot and knows five verbs\n"
        "(echo, mount, run, exec, need). Nothing on this machine will run a shell\n"
        "script from a file. If you want a sequence, type it, or put it in comments\n"
        "and read them -- which is what ~nomowner did twice.\n"
        "\n"
        "WHAT THE SHELL HAS. `help` prints most of it; the rest is here.\n"
        "\n"
        "  builtins        cd  pwd  bind  unbind  echo  help\n"
        "  for             for i in a b c; do ... ; done      $i expands\n"
        "  variables       NAME=value, $NAME, and $? for the last status\n"
        "  substitution    $(command) and `command`\n"
        "  redirection     > and >>\n"
        "  pipelines       a | b | c\n"
        "  and / or        && and ||\n"
        "  globbing        * and ? against a directory\n"
        "  quoting         single, double, and backslash\n"
        "\n"
        "There are no aliases, no functions, no if, and no arithmetic. The search\n"
        "path is inside the shell -- /bin, /usr/bin, /sbin, /usr/sbin, in that order\n"
        "-- and no profile changes it. Name anything else by its path.\n"
        "\n"
        "PIPELINES RUN TO COMPLETION, one stage at a time, because these are filters\n"
        "and a filter that has not finished has nothing to say. A builtin cannot be a\n"
        "stage, which is why /bin/echo exists as a real program as well.\n"
        "\n"
        "THE NETWORK: ip (addr, link, route, neigh), arp, ping, traceroute, ss,\n"
        "netstat and tcpdump. All of them read the running stack rather than the\n"
        "files that configure it, which is the whole point of them: the config is\n"
        "what somebody intended and these are what the machine has. `man` each.\n"
        "\n"
        "THE FILTERS: grep, sed, head, tail, wc, sort-of-everything-else via find.\n"
        "\n"
        "EDITING A FILE is ed(1), the line editor, and it is what every page\n"
        "that says `edit the file` means. It is not interactive -- nothing here\n"
        "can be, because a program runs to completion inside one command -- so\n"
        "the session is the argument list, one ed input line per argument:\n"
        "\n"
        "  ed /etc/fstab ,n\n"
        "  ed /etc/fstab 4d w\n"
        "\n"
        "`man ed`. For a single substitution `sed -i` is shorter, and it is the\n"
        "right tool on a file too big for an editor to hold:\n"
        "\n"
        "  sed -i s/testing/stable/ /etc/pkg/repos.d/main.repo\n"
        "  sed -i /badline/d /etc/fstab\n"
        "  echo \"a new line\" >> /etc/hosts\n"
        "\n"
        "Read /usr/share/doc/README for what else is documented, and `man` for the\n"
        "individual commands.\n", 0644, NULL },
      { "/usr/share/doc/nomsh/CHANGELOG",
        "nomsh CHANGELOG -- newest first.\n"
        "\n"
        "1.13 -- current. voice(8): the calls this machine has finished, and\n"
        "       why the worst of them broke up. Everything else in this package\n"
        "       reads the network NOW -- the sockets now, the neighbours now,\n"
        "       the card's counters now -- and a call is over by the time\n"
        "       anybody asks about it. Somebody sat at a desk whose day was\n"
        "       ruined by concealed audio and found ping, traceroute, ip addr\n"
        "       and netstat -P all reporting a machine in perfect health,\n"
        "       because it was: the audio was thrown away on somebody else's\n"
        "       port. The record outlives the stream, so the desk can now say\n"
        "       how much of its own audio never got played, and where.\n"
        "\n"
        "1.12 -- ed(1), the line editor. Three manual pages, this\n"
        "       shell's own `help` and the previous administrator's notes all\n"
        "       end with `edit the file`, and nothing on the machine could:\n"
        "       vi, ed, edit, nano and write all answered `command not found`.\n"
        "       `sed -i` changes a line that is already there and `echo >>`\n"
        "       adds one at the end; between them they cannot insert a line in\n"
        "       the middle, delete the third of four, or show you what you are\n"
        "       about to change. ed is on the rescue medium too, which is where\n"
        "       the repair it exists for actually happens.\n"
        "       Single quotes also stop $ expansion now, as they do in every\n"
        "       shell. They did not, so `$a` -- ed's address for the last line\n"
        "       -- could not be typed in any spelling at all.\n"
        "\n"
        "1.11 -- ip, arp, traceroute, ss and tcpdump. `netstat` and\n"
        "       `ping` were the whole of the toolbox inside a machine, and the\n"
        "       first person to build a network with it said so: one instrument,\n"
        "       and it cannot test reachability. These read the same running\n"
        "       stack netstat does. `ip addr`, `ip link`, `ip route`, `ip neigh`\n"
        "       in iproute2's shapes; `arp` with the card each neighbour answered\n"
        "       on and a real `arp -d`; `traceroute` counting real ttls off real\n"
        "       ICMP; `ss` for the sockets in the columns people type now; and\n"
        "       `tcpdump`, which is the one that changes what can be diagnosed --\n"
        "       the frames at this card, in both directions, with the fields that\n"
        "       were in the headers. On a pristine box `ping` says no answer and\n"
        "       `tcpdump icmp` shows the reply arriving, because the filter drops\n"
        "       it above IP. Nothing else on the machine can tell those apart.\n"
        "       Each one documents its subset: no `ip addr add`, no traceroute\n"
        "       times nobody measured, no `ss -p`, and a tcpdump filter it cannot\n"
        "       apply is refused by name rather than ignored.\n"
        "\n"
        "1.10 -- ping, the first program here that makes the machine TRY rather\n"
        "       than report what it believes. It reports the six answers the\n"
        "       stack really\n"
        "       produces rather than collapsing them into `no reply`: a router\n"
        "       with no route, a last hop with no arp answer, a ttl that ran out\n"
        "       and a packet that never left this box are four different repairs.\n"
        "\n"
        "1.9  -- seq and rev. The shell has `for` and no arithmetic, so until seq\n"
        "       there was no way to write a loop over a count, and therefore no way\n"
        "       to demonstrate inode exhaustion to yourself on a machine that was\n"
        "       not already broken.\n"
        "       The `for` word list and every tool's argument buffer are one command\n"
        "       line long and SAY SO when they overflow. Before this, `for i in\n"
        "       $(seq 1 300)` silently ran 154 times and `rm` with a hundred and\n"
        "       twenty matches silently removed none of them.\n"
        "\n"
        "1.8  -- Globbing, and `rm -r`. Deleting a directory of four hundred cache\n"
        "       files was previously impossible with the tools on the machine, which\n"
        "       made one entry in the fault list unsolvable rather than hard.\n"
        "\n"
        "1.7  -- tail, du and mkdir. /var/log/messages is half a megabyte and only\n"
        "       `head` existed, so the only end of the log a player could read was\n"
        "       the wrong one.\n"
        "\n"
        "1.6  -- Quoting. `sed -i s/enabled: yes/enabled: no/ f` was not awkward, it\n"
        "       was impossible in either quoting style, and a whole package had to be\n"
        "       reinstalled to change one word.\n"
        "\n"
        "1.5  -- Pipelines, and /bin/echo as a real program so it can be a stage.\n"
        "\n"
        "1.4  -- find and netstat, because everybody reached for them and they were\n"
        "       not there.\n", 0644, NULL },
    }, 63   /* RUNBOOK: 55 -> 63: /bin/rb, /bin/py and six examples */
};


/* ---------------------------------------------------------------------
 * A distribution is WIDE. `pkg verify` with no arguments over thirty-odd
 * packages is a wall of text with real local edits mixed into it -- so the
 * skill is knowing which package to suspect from where the boot died, and
 * verifying THAT. Dumping everything is a last resort, exactly as it is on a
 * real machine.
 * ------------------------------------------------------------------ */

/* The repository. `stable` is what this machine is built from; `testing`
 * carries a newer libc that nothing installed here is linked against. Point
 * the config at it, run `pkg upgrade`, and the machine breaks in a way that
 * is entirely the administrator's own doing -- which is what makes it fair. */
static const Package PKG_PKGCONF = {
    "pkg-config-data", "1.4", "the package manager's repositories",
    {
      { "/etc/pkg/repos.d/main.repo",
        "# the repository this machine is built from.\n"
        "# channels: stable (11.4) | testing (12.0-pre)\n"
        "name = main\n"
        "channel = stable\n"
        "url = https://packages.nomnix.org/11.4\n", 0644, NULL },
      { "/etc/pkg/pkg.conf",
        "# how aggressive upgrades are allowed to be\n"
        "allow_downgrade = no\n"
        "check_signatures = yes\n", 0644, NULL },
      { "/usr/share/doc/pkg-config-data/README",
        "pkg-config-data 1.4 -- where packages come from.\n"
        "\n"
        "  /etc/pkg/repos.d/main.repo   the repository, and THE CHANNEL\n"
        "  /etc/pkg/pkg.conf            how aggressive upgrades may be\n"
        "\n"
        "  channel = stable    11.4 -- what this machine is built from\n"
        "  channel = testing   12.0-pre -- what this machine is NOT built from\n"
        "\n"
        "THIS IS THE MOST DANGEROUS FILE ON THE MACHINE and it is three lines long.\n"
        "`pkg reinstall` and `pkg upgrade` both fetch from whatever this names. It is\n"
        "not on any boot path, no daemon reads it, and it will never appear in a\n"
        "console error, so it is the last file anybody thinks to look at and the\n"
        "first one worth ruling out when a repair does not take.\n"
        "\n"
        "  cat /etc/pkg/repos.d/main.repo\n"
        "\n"
        "before you reinstall anything, and it costs two seconds.\n"
        "\n"
        "pkg.conf: `allow_downgrade = no` means the package manager will not take you\n"
        "BACK across a version, which matters after an upgrade that should not have\n"
        "happened -- correct the channel, and then the reinstall does what you meant.\n", 0644, NULL },
      { "/usr/share/doc/pkg-config-data/known-issues",
        "pkg-config-data -- known issues. There is really only one and it is a\n"
        "masterpiece.\n"
        "\n"
        "THE CHANNEL IS POINTED AT testing.\n"
        "\n"
        "  Somebody changed one word, months ago, to get a fix. `pkg upgrade` then\n"
        "  fetched 12.0's libc: perfectly valid, correctly signed, not corrupt in any\n"
        "  way, and nothing on this machine is linked against it. Everything stops.\n"
        "\n"
        "  Now the part that makes it the best puzzle here:\n"
        "\n"
        "    pkg verify      reports the file as CHANGED. Correct.\n"
        "    pkg reinstall   fetches THE SAME WRONG VERSION straight back and\n"
        "                    reports \"4 files restored\". Also correct.\n"
        "\n"
        "  Neither tool is lying to you. Both are doing exactly what you asked. The\n"
        "  fault is three lines away in a config nobody thinks to look at, and the\n"
        "  repair is to fix the SOURCE and then reinstall:\n"
        "\n"
        "    cat /etc/pkg/repos.d/main.repo\n"
        "    sed -i s/testing/stable/ /etc/pkg/repos.d/main.repo\n"
        "    pkg reinstall libc\n"
        "\n"
        "  From the live medium when the libc is the casualty, because nothing on\n"
        "  that disk will run:\n"
        "\n"
        "    mount /dev/sda1 /mnt\n"
        "    cat /mnt/etc/pkg/repos.d/main.repo\n"
        "    pkg --root /mnt reinstall libc\n"
        "\n"
        "  THE DECOY VERSION OF THIS: a main.repo with the name, the url and the\n"
        "  comments all edited and the CHANNEL still saying stable. That is somebody\n"
        "  tidying, and it is harmless, and it looks identical at a glance. Read the\n"
        "  channel line. It is the only line that does anything.\n", 0644, NULL },
    }, 4
};

static const Package PKG_LIBC = {
    "libc", "2.38", "the C library",
    {
      { "/lib/libc.so.6",  "stub libc 2.38\n", 0755, NULL },
      { "/lib/libm.so.6",  "stub libm 2.38\n", 0755, NULL },
      { "/etc/ld.so.conf", "/lib\n/usr/lib\n", 0644, NULL },
      { "/etc/nsswitch.conf",
        "passwd: files\ngroup: files\nhosts: files dns\n", 0644, NULL },
      /* ldd ships with the C library on a real distribution, and for a real
       * reason: it has to agree with that library's loader about how a
       * dependency is resolved. Ours reads the same ELF section through the
       * same code the loader uses. */
      { "/usr/bin/ldd",    NULL, 0755, NULL },
      { "/usr/share/doc/libc/README",
        "libc 2.38 -- the C library.\n"
        "\n"
        "Everything on this machine is dynamically linked against it, which is not a\n"
        "detail: it is the reason a bad libc is the worst thing that can happen here.\n"
        "When libc is wrong, nothing on the disk runs, including every tool you would\n"
        "use to find out why, including the shell.\n"
        "\n"
        "WHAT THIS PACKAGE OWNS\n"
        "\n"
        "  /lib/libc.so.6      the library itself\n"
        "  /lib/libm.so.6      the maths library\n"
        "  /etc/ld.so.conf     the directories the loader searches, IN ORDER\n"
        "  /etc/nsswitch.conf  where name lookups go, in order: files then dns\n"
        "  /usr/bin/ldd        yes, really\n"
        "\n"
        "ldd SHIPS WITH THE C LIBRARY, here as on every real distribution, and for a\n"
        "real reason: it has to agree with this library's loader about how a\n"
        "dependency is resolved. Ours reads the same section of the ELF through the\n"
        "same code the loader uses, so it cannot disagree with what happens when you\n"
        "run the program. That is why `ldd` is evidence and not an opinion.\n"
        "\n"
        "  ldd /usr/sbin/httpd\n"
        "\n"
        "prints each library, THE PATH IT RESOLVED TO, and the version found there.\n"
        "The path is the important column and it is the one people skip.\n"
        "\n"
        "WHEN THE LIBC ITSELF IS THE CASUALTY\n"
        "\n"
        "You cannot fix it from the machine and you cannot chroot into it, because a\n"
        "chroot still runs that disk's programs. Boot the live medium, which carries\n"
        "its own libc and its own ldd, and work on the disk from outside it:\n"
        "\n"
        "  mount /dev/sda1 /mnt\n"
        "  ldd /mnt/usr/sbin/httpd\n"
        "  pkg --root /mnt verify libc\n"
        "  pkg --root /mnt reinstall libc\n"
        "\n"
        "`pkg --root` is deliberately not a chroot. That is the entire point of it.\n", 0644, NULL },
      { "/usr/share/doc/libc/CHANGELOG",
        "libc CHANGELOG -- newest first. `pkg list` reports the installed version;\n"
        "this file should start with it.\n"
        "\n"
        "2.38  -- current on the stable channel (11.4)\n"
        "        ldd prints the resolved PATH as well as the verdict, because \"not\n"
        "        found\" and \"found in the wrong directory\" were the same output and\n"
        "        are not the same fault.\n"
        "        Version comparison is done on the soname's recorded version rather\n"
        "        than on the filename, so renaming a library proves nothing.\n"
        "\n"
        "2.37  -- /etc/ld.so.conf is searched strictly in the order written. It\n"
        "        always was; it was not documented, and two administrators had\n"
        "        different beliefs about it in the same week.\n"
        "\n"
        "2.36  -- nsswitch honours `hosts: files dns` in order. Before this a name in\n"
        "        /etc/hosts and a name in DNS raced, which made \"it is DNS\" and \"it is\n"
        "        not DNS\" both true on alternate afternoons.\n"
        "\n"
        "12.0-pre -- ON THE TESTING CHANNEL ONLY. Not compatible with anything built\n"
        "        for 11.4, which is everything on this machine. See known-issues. It\n"
        "        is not broken, it is not corrupt, and it is signed. It is simply not\n"
        "        ours.\n", 0644, NULL },
      { "/usr/share/doc/libc/known-issues",
        "libc -- known issues, with what each looks like and what actually fixes it.\n"
        "\n"
        "1. THE WRONG LIBC ARRIVED FROM THE TESTING CHANNEL.\n"
        "\n"
        "   Symptom: nothing runs. Not the shell, not pkg, not ldd. 12.0's libc\n"
        "   wants a kernel newer than the one this release boots, and says so:\n"
        "   `FATAL: kernel too old` for whatever it was asked to start.\n"
        "   pkg verify: says libc's files CHANGED -- and it cannot run to say it.\n"
        "   TRAP: `pkg reinstall libc` fetches from the repository the machine is\n"
        "   configured for, and if /etc/pkg/repos.d still says `channel = testing`\n"
        "   that is the same wrong version again, reported as \"restored\".\n"
        "   Fix: correct /etc/pkg/repos.d/main.repo FIRST, then reinstall. From the\n"
        "   live medium, with `pkg --root /mnt`, because nothing on the disk runs.\n"
        "\n"
        "2. libc.so.6 IS THERE AND POINTS AT NOTHING.\n"
        "\n"
        "   A failed upgrade can leave the name as a symlink to a versioned file that\n"
        "   was removed. `ls /lib` shows the library, in the right directory, with\n"
        "   exactly the right name. `stat /lib/libc.so.6` says there is nothing at the\n"
        "   end of it.\n"
        "   The loader says `cannot open shared object file`, which is a DIFFERENT\n"
        "   SENTENCE from `version ... not found` and means a different thing: one is\n"
        "   \"I cannot find it\", the other is \"I found it and it is too old\". Read\n"
        "   which one you got before you decide what is wrong.\n"
        "\n"
        "3. A DIRECTORY MISSING FROM /etc/ld.so.conf.\n"
        "\n"
        "   The library is installed. `ls` finds it. Nothing can load it, because the\n"
        "   loader only looks in the directories that file lists. `ldd` reports it as\n"
        "   not found, which is the fault stated plainly, and the repair is one line\n"
        "   in one file rather than a package.\n"
        "\n"
        "4. TWO COPIES, AND THE ORDER DECIDES.\n"
        "\n"
        "   The worst of them, because NOTHING IS MISSING AND NOTHING IS CORRUPT. The\n"
        "   correct library is exactly where it belongs and is exactly right; there is\n"
        "   an older copy somewhere else, and /etc/ld.so.conf lists that somewhere\n"
        "   else FIRST. This is what unpacking a vendor tarball and making it work\n"
        "   leaves behind.\n"
        "\n"
        "     ldd /usr/sbin/httpd\n"
        "\n"
        "   prints the path it resolved to. When that path is not the one you expected\n"
        "   you have found it, and the repair is the ORDER of two lines and not the\n"
        "   content of either. `pkg verify` flags /etc/ld.so.conf, which reads exactly\n"
        "   like a deliberate local edit, because it is one.\n"
        "\n"
        "   Note carefully: a vendor path APPENDED to the end of that file is\n"
        "   harmless and somebody probably meant it. The same path at the TOP is the\n"
        "   fault. Same file, same line, different position.\n", 0644, NULL },
    }, 8
};

static const Package PKG_ZLIB = {
    "zlib", "1.3", "compression library",
    {
      { "/lib/libz.so.1", "\x7fELF (stub) zlib 1.3\n", 0755, NULL },
      { "/usr/share/doc/zlib/README",
        "zlib 1.3 -- compression.\n"
        "\n"
        "One file: /lib/libz.so.1.\n"
        "\n"
        "WHY THIS PACKAGE IS INTERESTING OUT OF ALL PROPORTION TO ITS SIZE\n"
        "\n"
        "Almost every program on this machine needs libc and nothing else, so almost\n"
        "every library fault breaks the whole machine at once and the diagnosis is\n"
        "over in one step. libz is needed only by the programs that compress what\n"
        "they write, and there are four of them on the entire disk:\n"
        "\n"
        "  /usr/sbin/httpd     compressed responses\n"
        "  /usr/sbin/auditd    a compressed audit trail\n"
        "  /usr/sbin/postfix   (shipped disabled on this machine)\n"
        "  /usr/bin/links      compressed transfers\n"
        "\n"
        "So a bad libz leaves the web server and the audit trail dead while ssh,\n"
        "cron, udev, ntp and the firewall run perfectly. That pattern -- two\n"
        "unrelated services down, everything else up -- is the shape you are meant to\n"
        "recognise. Everything dead means libc. TWO things dead means asking what\n"
        "those two have in common, and `ldd` on one of each answers it in seconds:\n"
        "\n"
        "  ldd /usr/sbin/httpd\n"
        "  ldd /usr/sbin/sshd\n"
        "\n"
        "The dead one lists a library the live one does not. That is the whole trick\n"
        "and it works for any pair.\n", 0644, NULL },
      { "/usr/share/doc/zlib/known-issues",
        "zlib -- known issues.\n"
        "\n"
        "1. AN OLDER libz, FOUND FIRST.\n"
        "\n"
        "   /lib/libz.so.1 is correct and present. There is a 1.2 in another\n"
        "   directory, and /etc/ld.so.conf lists that directory before /lib. The\n"
        "   loader takes the first one it finds, so httpd and auditd will not start\n"
        "   and everything that does not compress is completely fine.\n"
        "\n"
        "     ldd /usr/sbin/httpd\n"
        "         libc.so.6 => /lib/libc.so.6 (2.38)\n"
        "         libz.so.1 => /usr/lib/libz.so.1 (1.2)  -- TOO OLD\n"
        "\n"
        "   ldd prints the PATH for exactly this reason. `pkg owns` the stray copy and\n"
        "   nothing will own it. `pkg verify` flags /etc/ld.so.conf and not zlib,\n"
        "   because zlib is fine. The repair is the order of the lines.\n"
        "\n"
        "2. libz AT THE WRONG VERSION AFTER AN UPGRADE.\n"
        "\n"
        "   Same two services, same silence from everything else, but here `pkg verify\n"
        "   zlib` says CHANGED and the file really is the wrong one. Check the\n"
        "   repository channel before reinstalling -- if /etc/pkg/repos.d names\n"
        "   testing, reinstall will fetch the same thing back and tell you it\n"
        "   restored it.\n"
        "\n"
        "3. IT LOOKS LIKE A COINCIDENCE AND IT IS NOT.\n"
        "\n"
        "   The commonest wrong turn is treating \"httpd is down\" and \"auditd is down\"\n"
        "   as two tickets. They are one ticket with two symptoms, and the console\n"
        "   said the same sentence twice.\n", 0644, NULL },
    }, 3
};

static const Package PKG_CRON = {
    "cron", "3.0", "scheduled jobs",
    {
      { "/usr/sbin/crond", NULL, 0755, NULL },
      { "/etc/services.d/cron.svc",
        "# /etc/services.d/cron.svc\n"
        "name: cron\nexec: /usr/sbin/crond\n"
        "description: scheduled jobs\nafter: syslog\n"
        "restart: on-failure\nenabled: yes\nrunlevel: 3 5\n", 0644, NULL },
      { "/etc/crontab",
        "# m h dom mon dow  command\n"
        "#\n"
        "# Rules of this file, learned expensively:\n"
        "#   1. a job that has no log has never run\n"
        "#   2. a job you cannot run by hand is not a job, it is a rumour\n"
        "#   3. the line below with DISABLED on it is disabled. Read ~/TODO\n"
        "#      before you decide it looks harmless. -- nomowner\n"
        "17 *  * * *  /usr/sbin/logrotate /etc/logrotate.conf\n"
        "0  4  * * *  /home/nomowner/bin/cleanup   # DISABLED, see TODO\n", 0644, NULL },
      { "/var/spool/cron/root", "# no personal jobs\n", 0600, NULL },
          { "/var/spool/cron", NULL, 0755, NULL, true },
      { "/usr/share/doc/cron/README",
        "cron 3.0 -- scheduled jobs.\n"
        "\n"
        "  /usr/sbin/crond           the daemon\n"
        "  /etc/crontab              the system crontab\n"
        "  /var/spool/cron           A DIRECTORY THIS PACKAGE OWNS\n"
        "  /var/spool/cron/root      root's personal jobs, mode 0600\n"
        "  /etc/services.d/cron.svc  after syslog, runlevels 3 and 5\n"
        "\n"
        "THE FORMAT is five time fields and then the command:\n"
        "\n"
        "  m  h  dom mon dow  command\n"
        "  17 *  *   *   *    /usr/sbin/logrotate /etc/logrotate.conf\n"
        "\n"
        "crond writes a line to the system log every time it STARTS a job, which\n"
        "makes the log the cheapest answer to the first question anybody asks:\n"
        "\n"
        "  grep logrotate /var/log/messages\n"
        "  grep logrotate /var/log/messages.1\n"
        "\n"
        "A job with no log has never run. A job WITH a log has run, and if the thing\n"
        "it was supposed to accomplish has not happened, then the question is no\n"
        "longer \"does it fire\" but \"what does it actually do\" -- and those two\n"
        "questions have sent people down very different weeks.\n"
        "\n"
        "crond refuses to start with a crontab that has no jobs in it at all, rather\n"
        "than running with nothing to do and reporting itself healthy. It publishes\n"
        "/run/crond.state with the file it read.\n"
        "\n"
        "A COMMENTED LINE IS A WARNING SOMEBODY LEFT. There is one in /etc/crontab on\n"
        "this machine with DISABLED on it. Deleting a commented line is how the next\n"
        "person re-invents the thing it was warning about; leaving it costs nothing.\n", 0644, NULL },
      { "/usr/share/doc/cron/known-issues",
        "cron -- known issues.\n"
        "\n"
        "1. A JOB THAT DELETES THINGS.\n"
        "\n"
        "   This is not a bug in cron and cron will not protect you from it. An\n"
        "   unattended job with no dry run, no lock and no log is a loaded weapon in\n"
        "   a drawer. There is a real example on this machine: a tidy-up script that\n"
        "   removed a kernel image because a filename did not match a pattern its\n"
        "   author had written weeks earlier. Six hours.\n"
        "\n"
        "   Rules that came out of that, all of them cheap:\n"
        "     - a job you cannot run by hand is not a job, it is a rumour\n"
        "     - anything that deletes must be able to say what it WOULD delete\n"
        "     - if you disable it, comment the line and say why. Do not delete it.\n"
        "\n"
        "2. /var/spool/cron MISSING.\n"
        "\n"
        "   This package owns the directory, so `pkg verify cron` reports it and\n"
        "   `pkg reinstall cron` puts it back. Before packages recorded the\n"
        "   directories they own, this was diagnosable and not repairable, which is\n"
        "   the worst combination there is.\n"
        "\n"
        "3. crond RUNNING WITH THE OLD CRONTAB.\n"
        "\n"
        "   Editing /etc/crontab does not reach the running process. `cat\n"
        "   /run/crond.state` says which file it read; `kill -HUP <pid>` makes it\n"
        "   read it again.\n"
        "\n"
        "4. THE UNIT IS IN THE WRONG RUNLEVEL.\n"
        "\n"
        "   cron.svc says `runlevel: 3 5`. A unit that says only 5 on a machine that\n"
        "   boots to 3 is present, correct, enabled, healthy and never started, and\n"
        "   NOTHING reports an error, because nothing was tried. `enabled: yes` is\n"
        "   right there in the file and is the line everybody reads. The word\n"
        "   \"runlevel\" appears only on the console.\n", 0644, NULL },
    }, 7
};

static const Package PKG_LOGROTATE = {
    "logrotate", "3.21", "log rotation",
    {
      { "/usr/sbin/logrotate", "#!logrotate\n", 0755, NULL },
      { "/etc/logrotate.conf",
        "weekly\nrotate 4\ncompress\ninclude /etc/logrotate.d\n", 0644, NULL },
      { "/etc/logrotate.d/syslog",
        "/var/log/messages {\n  weekly\n  rotate 8\n}\n", 0644, NULL },
    }, 3
};

static const Package PKG_NTP = {
    "ntp", "4.2", "time synchronisation",
    {
      { "/usr/sbin/ntpd", NULL, 0755, NULL },
      { "/etc/ntp.conf",
        "server 10.0.2.3 iburst\ndriftfile /var/lib/ntp/drift\n", 0644, NULL },
      { "/etc/services.d/ntp.svc",
        "# /etc/services.d/ntp.svc\n"
        "name: ntp\nexec: /usr/sbin/ntpd\n"
        "description: time synchronisation\nafter: net\n"
        "restart: on-failure\nenabled: yes\nrunlevel: 3 5\n", 0644, NULL },
          { "/var/lib/ntp", NULL, 0755, NULL, true },
    }, 4
};

static const Package PKG_HTTPD = {
    "httpd", "2.4", "the web server",
    {
      { "/usr/sbin/httpd", NULL, 0755, NULL },
      /* THE DOCUMENT ROOT IS A DIRECTORY THIS PACKAGE OWNS.
       *
       * httpd now checks that the directory its config names is really there,
       * which means the directory can be deleted -- and a package cannot
       * restore a file whose directory is gone, because O_CREAT creates a
       * file and never a path. Owning it is what makes `pkg verify` say
       * `/srv/www missing` and `pkg reinstall` able to do anything about it.
       * Same bargain /run/nomde and /var/log already make. */
      { "/srv/www", NULL, 0755, NULL, true },
      { "/etc/httpd/httpd.conf",
        "Listen 80\nDocumentRoot /srv/www\nServerName nominal.local\n", 0644, NULL },
      { "/srv/www/index.html",
        "this machine\n============\n\n"
        "if you are reading this over the network, httpd is up and the\n"
        "document root is intact.\n", 0644, NULL },
      /* A README in the document root, which is where every real web server
       * has one. It is also the only place a note about httpd is certain to
       * be found by somebody who is already looking at httpd. */
      { "/srv/www/README",
        "This is DocumentRoot. /etc/httpd/httpd.conf says so, and httpd checks\n"
        "that this directory exists before it will start -- so if the web\n"
        "server is refusing to come up and the config looks perfect, ask\n"
        "whether the directory the config NAMES is still here.\n"
        "\n"
        "  grep DocumentRoot /etc/httpd/httpd.conf\n"
        "  ls /srv/www\n"
        "  svc status httpd\n"
        "  svc start httpd     once the directory is back. It gave up hours\n"
        "                      ago and nothing will retry it for you.\n"
        "\n"
        "Do not put anything secret in here. That is the entire point of the\n"
        "directory and people forget it about twice a year.\n"
        "\n"
        "-- nomowner. If the Listen line surprises you, read\n"
        "   /home/nomowner/Documents/handover.txt before you \"fix\" it.\n", 0644, NULL },
      { "/etc/services.d/httpd.svc",
        "# /etc/services.d/httpd.svc\n"
        "name: httpd\nexec: /usr/sbin/httpd\n"
        "description: web server\nafter: net\n"
        "restart: on-failure\nenabled: yes\nrunlevel: 3 5\n", 0644, NULL },
      { "/usr/share/doc/httpd/README",
        "httpd 2.4 -- the web server.\n"
        "\n"
        "  /usr/sbin/httpd            the daemon\n"
        "  /etc/httpd/httpd.conf      Listen, DocumentRoot, ServerName\n"
        "  /srv/www                   the document root, owned by this package\n"
        "  /srv/www/index.html        something to serve\n"
        "  /etc/services.d/httpd.svc  the unit: after net, restart on-failure\n"
        "\n"
        "WHAT IT DOES AT STARTUP, which is what makes it breakable honestly:\n"
        "\n"
        "  1. reads /etc/httpd/httpd.conf and refuses to start unless the file\n"
        "     names a DocumentRoot. A config that exists and does not say the\n"
        "     one thing its daemon needs is a different fault from a config\n"
        "     that is gone, and it fails later and less obviously.\n"
        "  2. STATS THE DIRECTORY DocumentRoot NAMES and refuses to start if it is\n"
        "     not there. A daemon that does not touch what its configuration points\n"
        "     at cannot be broken by pointing it somewhere wrong, so it touches it.\n"
        "  3. writes /run/httpd.state -- two lines: the config file it read, and\n"
        "     the first meaningful line it found in it, which in the shipped\n"
        "     config is the Listen line\n"
        "\n"
        "Point 3 is how \"running with a stale config\" becomes something the machine\n"
        "can notice rather than something only a person could spot. The file on disk\n"
        "says what httpd is SUPPOSED to do; /run/httpd.state says what the running\n"
        "process is really doing; `netstat` says the same thing from the other end.\n"
        "When they disagree, nobody reloaded it, and the fix is a signal:\n"
        "\n"
        "  cat /run/httpd.state\n"
        "  netstat\n"
        "  ps\n"
        "  kill -HUP <pid>            or, by name, `svc reload httpd`\n"
        "\n"
        "Either of those keeps the process up, which is the point: a reboot\n"
        "also fixes it and takes the evidence with it, so you never learn what\n"
        "was wrong. `svc restart httpd` re-reads the unit file as well, and is\n"
        "what you want when the daemon is not the one holding the old copy.\n"
        "\n"
        "ON THE LISTEN PORT: this machine is shipped listening on 80. Sites where a\n"
        "load balancer terminates set it to 8080 on purpose, and that edit shows up\n"
        "in `pkg verify` as CHANGED for ever afterwards. It is a decision, not a\n"
        "fault. `pkg diff` it and read it before you \"correct\" it.\n"
        "\n"
        "It needs libz, so a library problem takes it down together with auditd and\n"
        "leaves everything else running. See /usr/share/doc/zlib/README.\n", 0644, NULL },
      { "/usr/share/doc/httpd/CHANGELOG",
        "httpd CHANGELOG -- newest first.\n"
        "\n"
        "2.4  -- current.\n"
        "       Stats DocumentRoot at startup and refuses to run without it. Before\n"
        "       this it read the directive and never looked at what it named, so\n"
        "       /srv/www could be deleted and httpd would come up and serve nothing\n"
        "       and report itself perfectly healthy.\n"
        "       Publishes /run/httpd.state with the config path and the port loaded.\n"
        "       Re-reads its configuration on SIGHUP.\n"
        "\n"
        "2.3  -- Refuses to start when the config names no DocumentRoot, rather\n"
        "       than defaulting to somewhere. A daemon that invents a value for a\n"
        "       line you commented out has hidden your mistake instead of showing\n"
        "       it to you.\n"
        "\n"
        "2.2  -- Compressed responses. This is where the libz dependency came from,\n"
        "       and where \"httpd and auditd are both down and nothing else is\"\n"
        "       started being a sentence worth recognising.\n"
        "\n"
        "2.0  -- Initial packaging for NomnixOS 11.\n", 0644, NULL },
      { "/usr/share/doc/httpd/known-issues",
        "httpd -- known issues.\n"
        "\n"
        "1. THE DOCUMENT ROOT IS NOT THERE.\n"
        "\n"
        "   The config is valid, the binary is fine, the machine boots all the way to\n"
        "   a login prompt, and the web server is dead because the directory its\n"
        "   configuration names has been moved or deleted.\n"
        "\n"
        "     grep DocumentRoot /etc/httpd/httpd.conf\n"
        "     ls /srv/www\n"
        "     svc status httpd\n"
        "     svc start httpd      after the directory is back. A service that\n"
        "                          gave up stays given up until it is told.\n"
        "\n"
        "   /srv/www is a directory THIS PACKAGE OWNS, which is what makes it\n"
        "   repairable: `pkg verify httpd` reports it missing and `pkg reinstall\n"
        "   httpd` puts it back. A package cannot restore a file into a directory\n"
        "   that does not exist -- creating a file never creates a path -- so\n"
        "   owning the directory is the difference between a fault and a dead end.\n"
        "\n"
        "2. RUNNING, AND ON THE WRONG PORT.\n"
        "\n"
        "   `svc` says running. `pkg verify` is clean. The config plainly says one\n"
        "   port and the machine is answering on another. Nothing is corrupt: a\n"
        "   daemon reads its configuration ONCE, at startup, and somebody edited the\n"
        "   file afterwards.\n"
        "\n"
        "     cat /run/httpd.state     what it actually loaded\n"
        "     netstat                  what it is actually listening on\n"
        "     kill -HUP <pid>          make it re-read\n"
        "\n"
        "   This one evaporates if you reboot, which is why it is so miserable to\n"
        "   catch in life and why rebooting is a diagnostic and not a repair.\n"
        "\n"
        "3. DOWN TOGETHER WITH auditd AND NOTHING ELSE.\n"
        "\n"
        "   That is libz, every time. `ldd /usr/sbin/httpd` and `ldd /usr/sbin/sshd`\n"
        "   and compare. See /usr/share/doc/zlib/known-issues.\n"
        "\n"
        "4. THE UNIT NAMES A PATH THE BINARY HAS NEVER BEEN AT.\n"
        "\n"
        "   `svcinit` says `not found` and the binary is present, correct and\n"
        "   executable exactly where this package put it. What is wrong is the\n"
        "   pointer, and `pkg verify httpd` flags the .svc file and not the program,\n"
        "   which is the clue.\n", 0644, NULL },
    }, 9
};

static const Package PKG_FIREWALL = {
    "nftables", "1.0", "packet filter",
    {
      { "/usr/sbin/nft", NULL, 0755, NULL },
      { "/etc/nftables.conf",
        "table inet filter {\n"
        "  chain input {\n"
        "    type filter hook input priority 0; policy drop;\n"
        "    tcp dport { 22, 80 } accept\n"
        "  }\n}\n", 0644, NULL },
      { "/etc/services.d/nftables.svc",
        "# /etc/services.d/nftables.svc\n"
        "name: nftables\nexec: /usr/sbin/nft\n"
        "description: packet filter\ncritical: yes\nafter: udev\n"
        "restart: on-failure\nenabled: yes\nrunlevel: 3 5\n", 0644, NULL },
    }, 3
};

static const Package PKG_MAN = {
    "man-db", "2.11", "the manual",
    {
      { "/usr/bin/man", NULL, 0755, NULL },
      { "/usr/share/man/pkg",
        "pkg(1)\n\n"
        "  pkg list                 every installed package\n"
        "  pkg verify [name]        compare installed files against the\n"
        "                           manifest in /var/lib/pkg/<name>/files\n"
        "  pkg --root DIR <verb>    work on a filesystem mounted somewhere\n"
        "                           else, WITHOUT chrooting into it. The only\n"
        "                           way in when the disk's own libc is broken\n"
        "                           and nothing on it will run.\n"
        "  pkg reinstall <name>     put a package's files back. Config files\n"
        "                           that were edited on this machine are LEFT\n"
        "                           ALONE -- somebody chose those settings.\n"
        "  pkg reinstall --force <name>\n"
        "                           overwrite them too. The old file is kept\n"
        "                           as <path>.pkgsave, so\n"
        "                           `pkg diff` them first. NOTE that\n"
        "                           /etc/hostname belongs to `filesystem`, so\n"
        "                           forcing that package renames the machine\n"
        "                           back to its factory name. It says so when\n"
        "                           it does it.\n"
        "  pkg diff <path>          what a CHANGED file says, against what\n"
        "                           the package shipped\n"
        "  pkg owns <path>          which package owns it\n"
        "  pkg reinstall <name>     refetch from the repository\n"
        "\n"
        "Every package that has anything worth saying ships its documentation\n"
        "at /usr/share/doc/<name> -- a README, a CHANGELOG whose top entry is\n"
        "the version above, and known-issues where the package has any. Start\n"
        "with `ls /usr/share/doc` and `cat /usr/share/doc/README`.\n"
        "\n"
        "WHAT VERIFY SAYS ABOUT A FILE.\n"
        "  MISSING      it is not there\n"
        "  CHANGED      the bytes are different bytes\n"
        "  TRUNCATED    the bytes that are there ARE the shipped bytes, and\n"
        "               then the file stops. That is the shape an interrupted\n"
        "               write leaves, and verify prints how much is gone.\n"
        "               A reinstall is the repair -- with --force if it is\n"
        "               under /etc, which a plain reinstall would keep.\n"
        "  MODE         right contents, wrong permissions\n"
        "  REPOINTED    a symlink pointing somewhere it did not ship pointing\n"
        "\n"
        "NOTE. `pkg verify` with no arguments checks EVERY package and will\n"
        "report local configuration changes as CHANGED, because they are.\n"
        "That is not a fault list. Work out which package to suspect from\n"
        "where the boot stopped, verify that one, and use `pkg diff` before\n"
        "you reinstall anything.\n", 0644, NULL },
      { "/usr/share/man/boot",
        "boot(7)\n\n"
        "  firmware -> zbl -> kernel -> initrd -> init -> rc -> services\n"
        "\n"
        "  zbl              /boot/zbl/zbl.cfg        pkg zbl\n"
        "  kernel, initrd   /boot/vmnomuz, /boot/initrd (SYMLINKS)\n"
        "                                            pkg kernel-default\n"
        "  init             /sbin/init, /etc/inittab pkg sysinit\n"
        "  rc               /etc/rc.boot, /etc/rc.d  pkg sysinit\n"
        "  services         /etc/services.d/*.svc    the owning package\n"
        "\n"
        "The stage the console stops at tells you which package to verify.\n", 0644, NULL },
      /* THE LOADER GAINED A GRAMMAR, so it needs a page. Everything under
       * /usr/share/doc/zbl is the long version; this is what somebody wants
       * while they are looking at the file. */
      { "/usr/share/man/zbl",
        "zbl(8)\n\n"
        "  /boot/zbl/zbl.cfg          the only file the loader reads\n"
        "  zbl-mkconfig               write it from THIS machine\n"
        "  zbl-install [/dev/sda]     write the boot sector, and point the\n"
        "                             firmware at the disk it wrote it to\n"
        "\n"
        "One directive per line. Leading space is ignored, blank lines are\n"
        "ignored, # is a comment. SIX directives and no others -- anything\n"
        "else stops the boot and names the line it was on:\n"
        "\n"
        "  default N   which entry, COUNTED FROM ZERO. Absent means 0.\n"
        "  timeout N   seconds. Read, kept, and never waited on: nothing on\n"
        "              this machine can press a key during a boot.\n"
        "  entry LABEL opens a block, which runs to the next `entry` or to\n"
        "              the end of the file. The label is for you; the loader\n"
        "              identifies an entry by its number.\n"
        "  kernel PATH  \\\n"
        "  initrd PATH   > all three required, INSIDE the entry\n"
        "  root SPEC    /  UUID=<uuid>, or a device node.\n"
        "\n"
        "Lines above the first `entry` are global. kernel, initrd and root\n"
        "are read from the CHOSEN ENTRY ONLY, so a `kernel` line in the\n"
        "global section is read by nothing and the loader says there is no\n"
        "kernel line while you are looking straight at one.\n"
        "\n"
        "  default 0\n"
        "  timeout 5\n"
        "\n"
        "  entry \"NomnixOS 11.4\"\n"
        "    kernel /boot/vmnomuz\n"
        "    initrd /boot/initrd\n"
        "    root UUID=8f41-2c07-a19d-5be3\n"
        "\n"
        "`zbl: booting entry 0 of 2` says which one it took. On a config with\n"
        "more entries than the machine has kernels, read that line before you\n"
        "read the error underneath it.\n"
        "\n"
        "The boot sector is NOT a file: no package owns it and `pkg verify`\n"
        "cannot see it. zbl-install is the only thing that writes one, and it\n"
        "refuses when there is no zbl.cfg to install a loader for.\n"
        "\n"
        "/usr/share/doc/zbl has the failures and their repairs; boot(7) has\n"
        "the order of the stages.\n", 0644, NULL },
      { "/usr/share/man/rescue",
        "rescue(7)\n\n"
        "  mount /dev/sda1 /mnt\n"
        "  for i in dev sys proc; do mount /$i /mnt/$i; done\n"
        "  chroot /mnt\n"
        "\n"
        "The customer disk is /dev/sda1. The live medium is /dev/sr0 and is\n"
        "never damaged. `exit` leaves the chroot; `quit` hangs up.\n", 0644, NULL },
      /* THE COMMAND THIS MACHINE'S SECOND HALF IS MADE OF HAD NO PAGE.
       *
       * A blind playtester worked a whole shift of running-and-wrong tickets
       * with `svc` and could only find the verbs by typing a wrong one and
       * reading the usage line. Half of them did not exist yet, which is the
       * other half of the same bug. */
      /* THREE TOOLS THE SHELL'S OWN HELP NAMES AND THE MANUAL DID NOT.
       * Written from what these programs actually print on a booted machine,
       * not from what their Linux namesakes do -- netstat here has six
       * options and no -a, -n or -p, and saying otherwise would send a
       * player hunting for a flag that does not exist. */
      { "/usr/share/man/netstat",
        "netstat(8)\n\n"
        "  netstat            the SOCKETS: what is listening, and to whom\n"
        "  netstat -i         the interfaces, as the kernel holds them\n"
        "  netstat -r         the routing table\n"
        "  netstat -A         the arp cache\n"
        "  netstat -P         the network PORT: link, tx, rx, drops\n"
        "  netstat -F         the firewall, and what each rule has dropped\n"
        "  netstat -W         start capturing frames\n"
        "  netstat -w         print what has been captured since -W\n"
        "That is the whole list. There is no -a, -n or -p on this machine,\n"
        "and an option it does not know is refused by name.\n"
        "\n"
        "WHY THE BARE FORM IS THE INTERESTING ONE. These are REAL sockets. A\n"
        "service that died has none, so a unit `svc` calls running and a port\n"
        "missing here is a service that came up and fell over. And a service\n"
        "started before somebody edited its config is listening on the port\n"
        "it LOADED, not the port the file now says -- so\n"
        "  netstat                              says 80\n"
        "  grep Listen /etc/httpd/httpd.conf    says 8080\n"
        "is not a contradiction. It is a config edited after the service\n"
        "started, and `svc reload httpd` is what closes it.\n"
        "\n"
        "AN EMPTY TABLE SAYS WHAT IT MEANS rather than printing a header and\n"
        "nothing: `the routing table is empty -- this machine cannot send\n"
        "anywhere`, `the arp cache is empty -- nothing on this wire has\n"
        "answered yet`. Those two sentences are different diagnoses and the\n"
        "difference matters: no route is configuration, no arp is the wire.\n"
        "\n"
        "See also ip(8), which SHOWS the same tables and cannot change them,\n"
        "ss(8) for sockets alone, and tcpdump(8) for the frames themselves.\n",
        0644, NULL },

      { "/usr/share/man/blkid",
        "blkid(8)\n\n"
        "  blkid              every block device, its UUID and its type\n"
        "\n"
        "  /dev/sda1: UUID=\"8f41-2c07-a19d-5be3\" TYPE=\"ext4\"\n"
        "  /dev/sr0:  no medium (the drive is empty)\n"
        "\n"
        "WHAT IT IS FOR HERE. /etc/fstab and /boot/zbl/zbl.cfg both name the\n"
        "root filesystem BY UUID, not by device. So when the loader says it\n"
        "cannot find the root, or the boot stops on a mount, the question is\n"
        "whether the UUID those files ask for is the UUID the disk has -- and\n"
        "this is the only thing that will tell you what the disk has.\n"
        "\n"
        "  blkid                      what the disk says it is\n"
        "  grep UUID /etc/fstab       what the mount asks for\n"
        "  cat /boot/zbl/zbl.cfg      what the loader asks for\n"
        "\n"
        "An empty optical drive says so plainly, which matters because a boot\n"
        "order still pointing at /dev/sr0 with nothing in it is a real fault\n"
        "and looks nothing like a broken disk. See also fsck(8), zbl(8).\n",
        0644, NULL },

      { "/usr/share/man/dmesg",
        "dmesg(8)\n\n"
        "  dmesg              the console log of this boot, from the top\n"
        "\n"
        "Everything the firmware, the loader, the kernel, the initrd and\n"
        "svcinit printed while this machine was coming up -- the same text\n"
        "that went past on the console, kept so you can read it after the\n"
        "fact. On a box you reached over the service processor after it had\n"
        "already booted, this is the only place that boot exists.\n"
        "\n"
        "IT IS WHERE THE FAULT USUALLY NAMES ITSELF. The boot chain stops at\n"
        "the stage that is actually wrong and says which file it could not\n"
        "use, so the first move on a machine that is up but not right is to\n"
        "read this from the top rather than to guess:\n"
        "  zbl: /boot/zbl/zbl.cfg: not found        the loader\n"
        "  initrd: UNEXPECTED INCONSISTENCY         the filesystem -- fsck\n"
        "  rc: /opt/monitoring/bin/probe: not found a boot script\n"
        "  kernel: httpd respawning too fast,       a service that starts\n"
        "          giving up on it                  and immediately dies\n"
        "\n"
        "It is this boot only. Nothing here survives a reboot, so read it\n"
        "BEFORE power-cycling a machine -- a reboot is how the evidence gets\n"
        "thrown away. What syslog kept is in /var/log. See also svc(1),\n"
        "fsck(8), pkg(1).\n",
        0644, NULL },

      /* THE PAGE FOR THE TOOL THE INITRD TELLS YOU TO RUN. A playtester
       * repaired a mains-damaged filesystem here and said afterwards that
       * they only knew to type `fsck` because the boot had named it -- and
       * `man fsck` answered "no manual entry", which is the machine telling
       * you to use a tool it will not explain. Every line below is what
       * machine_fsck() in core/kernel.c actually does. */
      { "/usr/share/man/fsck",
        "fsck(8)\n\n"
        "  fsck <device>                check and repair a filesystem\n"
        "  fsck                         the same, on /dev/sda1\n"
        "\n"
        "IT MUST NOT BE MOUNTED. The root filesystem is mounted while the\n"
        "machine is running, so the way you get here is the rescue medium:\n"
        "the boot stops, you plug in the service processor, and the disk is\n"
        "/dev/sda1 and unmounted. /dev/sda1 and /dev/sda are the only two\n"
        "devices this understands; anything else is refused by name.\n"
        "\n"
        "A CLEAN FILESYSTEM SAYS SO and does nothing:\n"
        "  /dev/sda1: clean\n"
        "so running it on a healthy machine costs you nothing but a line.\n"
        "\n"
        "A DIRTY ONE was interrupted mid-write -- the mains went, or a box\n"
        "was switched off while it was running. It recovers the journal and\n"
        "walks the passes, and clears the dirty flag at the end:\n"
        "  /dev/sda1: recovering journal\n"
        "  Pass 1: checking inodes, blocks, and sizes\n"
        "  Pass 2: checking directory structure\n"
        "  Pass 5: checking group summary information\n"
        "  /dev/sda1: FILE SYSTEM WAS MODIFIED\n"
        "\n"
        "IT IS HALF THE REPAIR, AND IT SAYS WHICH HALF. fsck rebuilds\n"
        "METADATA. It cannot rebuild the CONTENTS of a file that was being\n"
        "written when the power went, so those inodes are cleared and\n"
        "reported:\n"
        "  Pass 4: 3 inode(s) with bad content, cleared\n"
        "and then it tells you plainly to go and look:\n"
        "  fsck repaired the filesystem. It could not repair the\n"
        "  CONTENTS of what was being written -- check the packages.\n"
        "\n"
        "THAT SECOND HALF IS `pkg verify`. The files fsck cleared are the\n"
        "ones it will report, and `pkg diff <path>` tells damage from an\n"
        "edit somebody made on purpose before you reinstall over it.\n"
        "\n"
        "So the whole procedure after a power cut is: boot the rescue\n"
        "medium, `fsck /dev/sda1`, boot the disk, `pkg verify`, then repair\n"
        "what it names. See also pkg(1), rescue(7), boot(7).\n",
        0644, NULL },

      { "/usr/share/man/svc",
        "svc(1)\n\n"
        "  svc                          every unit and its state\n"
        "  svc status <name>            why THIS one is unhappy\n"
        "\n"
        "  svc start <name>             start it now\n"
        "  svc stop <name>              stop it now\n"
        "  svc restart <name>           stop it, then start it -- which\n"
        "                               re-reads the unit AND the config\n"
        "  svc reload <name>            ask it to re-read its config without\n"
        "                               going down. A HUP by another name\n"
        "\n"
        "  svc enable <name>            start it at the NEXT boot\n"
        "  svc disable <name>           do not\n"
        "\n"
        "THE TOP HALF IS NOW, THE BOTTOM HALF IS NEXT TIME, and a repair is\n"
        "usually one of each: `svc enable httpd` alone leaves the machine\n"
        "still down, and `svc start httpd` alone leaves it fixed until\n"
        "somebody reboots it.\n"
        "\n"
        "reload is not restart. A daemon that keeps its configuration in\n"
        "memory reads the file once, at startup, so an edited file changes\n"
        "nothing until it is told -- that is the fault where pkg verify is\n"
        "clean and the machine still does the wrong thing. reload picks the\n"
        "new file up with the process still running, which is the only fix\n"
        "that does not throw away the evidence. Not every daemon listens:\n"
        "the ones that do not say so, and restart is the answer for them.\n"
        "\n"
        "Stopping a service is a change to the machine like any other. `svc`\n"
        "will show it stopped, netstat will show its port gone, and handing\n"
        "the ticket back will fail on it -- which is right.\n", 0644, NULL },
      { "/usr/share/man/ns",
        "ns(1)\n\n"
        "  bind TARGET AT     lookups under AT resolve to TARGET\n"
        "  ns [pid]           print a namespace\n"
        "\n"
        "A bad bind is a fault where nothing is corrupt: every file passes\n"
        "pkg verify and the machine still reads the wrong one.\n", 0644, NULL },
      { "/usr/share/man/ldd",
        "ldd(1)\n\n"
        "  ldd <program>      the libraries a program needs, where each one\n"
        "                     was found, and whether it is new enough\n"
        "\n"
        "Resolution follows /etc/ld.so.conf in order, exactly as the loader\n"
        "does, so ldd cannot disagree with what happens when you run the\n"
        "program. A library that is installed but sits in a directory nobody\n"
        "lists reads as `not found`, which is the fault, stated plainly.\n"
        "\n"
        "Not every program needs the same libraries. When some services are\n"
        "dead and others are fine, ldd on one of each is the fastest way to\n"
        "see what the dead ones have in common.\n"
        "\n"
        "From the rescue medium, name the broken binary through the mount:\n"
        "  ldd /mnt/usr/sbin/httpd\n"
        "That works even when the disk's own libc is too broken to run\n"
        "anything at all, which is when you need it most.\n", 0644, NULL },
      /* du, tail and mkdir are new on this machine, and a new command with
       * no page is a command nobody finds. Each page states what the flags
       * ARE, because this userland refuses the ones it does not have rather
       * than accepting and ignoring them -- so a page that over-promises
       * would send someone into an error message. */
      { "/usr/share/man/du",
        "du(1)\n\n"
        "  du [-s] [-h] [dir ...]\n"
        "    -s   the total only, no line per subdirectory\n"
        "    -h   K and M rather than K throughout\n"
        "\n"
        "du AGREES WITH df, deliberately: same bytes, same divisor, no\n"
        "rounding each file up to a block. `du -s /` and the USED column of\n"
        "`df` are the same number, so when they differ the disagreement is\n"
        "real and worth chasing rather than an artefact of the tools.\n"
        "\n"
        "Directories cost no bytes here; they cost an inode, which is what\n"
        "`df -i` counts. Symlinks are not followed and cost nothing. /proc is\n"
        "skipped because it is generated and df cannot see it either.\n"
        "\n"
        "On a full disk: `df` first, then `du -s /var /usr /home` to find the\n"
        "branch, then du again inside it.\n", 0644, NULL },
      { "/usr/share/man/tail",
        "tail(1)\n\n"
        "  tail [-n N] [-N] [file ...]      the last N lines, ten by default\n"
        "\n"
        "There is no -f. Nothing on this machine runs while the shell is\n"
        "waiting for you to type, so a follow would follow nothing; the flag\n"
        "is refused rather than accepted and left to sit there.\n"
        "\n"
        "It keeps the last 16 KB of the file as the file streams past, so it\n"
        "costs the same on a 524 KB /var/log/messages as on a config, and it\n"
        "says so if the lines you asked for ran off the front of what it\n"
        "kept. `dmesg` is the boot log; tail is for everything else.\n", 0644, NULL },
      { "/usr/share/man/mkdir",
        "mkdir(1)\n\n"
        "  mkdir [-p] <dir> ...\n"
        "    -p   make the parents too, and treat an existing directory as\n"
        "         success rather than as an error\n"
        "\n"
        "Without -p the directory above it must already exist. That is not\n"
        "pedantry: `open` on this machine refuses to invent directories, so a\n"
        "typo that quietly created /var/lgo would leave a tree that looks\n"
        "right and that nothing writes to.\n"
        "\n"
        "It fails when the parent is not writable, when the root is mounted\n"
        "read-only, and when the filesystem is out of inodes -- `ls -ld` on\n"
        "the parent, `mount`, and `df -i` respectively.\n", 0644, NULL },
      { "/usr/share/man/seq",
        "seq(1)\n\n"
        "  seq LAST                 1 .. LAST\n"
        "  seq FIRST LAST           FIRST .. LAST\n"
        "  seq FIRST STEP LAST      FIRST, FIRST+STEP, ... not past LAST\n"
        "\n"
        "One number per line. A negative STEP counts down; a STEP of zero is\n"
        "refused, because nothing on this machine can interrupt a loop that\n"
        "never ends. An empty range prints nothing and succeeds, so\n"
        "`for i in $(seq 1 0)` runs zero times rather than failing.\n"
        "\n"
        "This shell has `for` and `$(...)` and no arithmetic, so seq is how a\n"
        "count becomes a loop. A loop takes at most 256 words and says so if\n"
        "it is given more, so a big count is two loops:\n"
        "\n"
        "  for i in $(seq 1 250); do touch /tmp/a$i; done\n"
        "  for i in $(seq 1 250); do touch /tmp/b$i; done\n"
        "  df\n"
        "  df -i\n"
        "\n"
        "Do that on a healthy machine and watch the two answers come apart.\n"
        "`df` still reports about half a megabyte free -- five hundred empty\n"
        "files cost no bytes worth counting -- and `touch` cannot create one\n"
        "more, because they cost an inode each and the inodes are gone. That\n"
        "is the whole of why `df` and `df -i` are two different questions, and\n"
        "it is worth doing once on a machine that is not broken so that you\n"
        "recognise it on one that is.\n"
        "\n"
        "  rm /tmp/a*\n"
        "  rm /tmp/b*\n"
        "\n"
        "puts every one of them back.\n",
        0644, NULL },
      /* THE PAGE THAT MATTERS MOST ON A NETWORK. ping is the only tool that
       * makes the machine try, and the value is entirely in the fact that it
       * answers six different ways. A page that said "it pings" would leave
       * the player treating four different repairs as one dead end. */
      { "/usr/share/man/ping",
        "ping(1)\n\n"
        "  ping [-c count] <address or name>     three by default\n"
        "\n"
        "A real echo request, out of this machine's card, down whatever cable\n"
        "is in it. Everything else here reads state -- `netstat -i` says what\n"
        "address the card HAS, `-r` what the table SAYS, `-A` who has\n"
        "answered before, `ip` and `arp` and `ss` the same things in the\n"
        "shapes iproute2 prints them. This is the one that tries, and the one\n"
        "that measures: traceroute(8) also sends, but it counts hops and does\n"
        "not time them.\n"
        "\n"
        "WHAT IT CAN SAY, and each line is a different repair:\n"
        "\n"
        "  reply ... time=N ms          it works.\n"
        "  no answer                    it left this machine and nothing came\n"
        "                               back. A filter dropping it, or a box\n"
        "                               that is not running. A pristine machine\n"
        "                               ships `policy drop` and no rule for\n"
        "                               icmp, so it will not answer one --\n"
        "                               `netstat -F` on the far end, if you can\n"
        "                               reach it, and nft(8) for the rule.\n"
        "  destination net unreachable  a ROUTER on the path had no route for\n"
        "                               it and said so. The fault is past your\n"
        "                               gateway, not on this box.\n"
        "  destination host unreachable the last hop got no arp answer: that\n"
        "                               address is on that wire and nothing on\n"
        "                               it holds it. Usually a typo in an\n"
        "                               address or a mask.\n"
        "  time exceeded in transit     the ttl ran out. Two routers are\n"
        "                               pointing at each other.\n"
        "  network is unreachable       nothing was SENT: this machine's own\n"
        "                               routing table has nothing to try.\n"
        "                               `netstat -r`, and check the gateway.\n"
        "  interface ... is down        no carrier or no address, so again\n"
        "                               nothing was sent. `netstat -P`.\n"
        "\n"
        "The last two are local and the first four are not, which is the first\n"
        "cut to make: they tell you whether to keep looking at this machine.\n"
        "\n"
        "A NAME IS RESOLVED FIRST, and its failure is reported separately, so\n"
        "`cannot resolve` is never confused with `no answer` -- a broken\n"
        "resolver and a broken route look identical otherwise.\n", 0644, NULL },
      /* THE INSTRUMENTS, AND THE SUBSET OF EACH THAT IS REAL. A man page
       * that documents a flag this machine does not have is the same lie as
       * a tool that prints an answer it did not measure, so each of these
       * describes exactly what was implemented and names what was not. */
      { "/usr/share/man/ip",
        "ip(8)\n\n"
        "  ip addr [show]     the addresses and masks the cards really hold\n"
        "  ip link [show]     the cards: mac, admin state, carrier\n"
        "  ip route [show]    the table the kernel really looks in\n"
        "  ip neigh [show]    the arp cache: who has really answered\n"
        "\n"
        "`a`, `l`, `r` and `n` are accepted, as they are in iproute2.\n"
        "\n"
        "IT SHOWS AND IT DOES NOT CONFIGURE. There is no `ip addr add`, no\n"
        "`ip link set`, no `ip route add`, and asking for one says so rather\n"
        "than failing quietly. An address on this machine comes from\n"
        "/etc/net/interfaces by way of netd, and netd re-reads that file when\n"
        "it changes -- so an address set from a command line would be undone\n"
        "the next time anything touched the config, which is worse than not\n"
        "having the command. Edit the file, `svc reload net`, then `ip addr`\n"
        "-- and the editor is ed(1), which is on this machine and on the\n"
        "rescue medium:\n"
        "\n"
        "  ed /etc/net/interfaces ,n\n"
        "  svc reload net\n"
        "\n"
        "WHAT THE FLAGS IN THE ANGLE BRACKETS MEAN. Two states, and they are\n"
        "two different faults:\n"
        "\n"
        "  UP / DOWN            the INTERFACE, which is administrative\n"
        "  LOWER_UP             carrier: there is a cable and the far end is on\n"
        "  NO-CARRIER           the port has no link. Nothing is plugged in,\n"
        "                       the far end is off, or the run is too long --\n"
        "                       `netstat -P` says which\n"
        "\n"
        "`mtu 1500` is a constant of this machine and not a per-card setting:\n"
        "the stack refuses a payload over 1500 bytes on every interface, and\n"
        "there is nothing to set.\n"
        "\n"
        "ip neigh prints REACHABLE or STALE, and that is not decoration: the\n"
        "stack asks again for any entry older than 120 seconds before it uses\n"
        "it, so those are the two things an entry can really be. arp(8) prints\n"
        "the same cache with the age in it.\n"
        "\n"
        "IT HAS NO FILTER. `ip addr show eth0` is refused rather than\n"
        "ignored; it prints every interface.\n", 0644, NULL },
      { "/usr/share/man/arp",
        "arp(8)\n\n"
        "  arp                 the neighbour cache, one line per entry\n"
        "  arp -a              the BSD spelling of the same thing\n"
        "  arp <address>       just that one\n"
        "  arp -d <address>    forget one neighbour\n"
        "  arp -n              numeric, which is the only thing this arp is\n"
        "\n"
        "THREE FAULTS LIVE IN THIS TABLE and they look different here:\n"
        "\n"
        "  an entry with a mac    that neighbour answered. Whatever else is\n"
        "                         wrong, the wire between you and it works.\n"
        "  (incomplete)           you asked and nothing answered. That address\n"
        "                         is on this wire and no card holds it: a typo\n"
        "                         in an address or a mask, or a box that is off.\n"
        "  the WRONG mac          two machines answered for one address and the\n"
        "                         cache believes whichever spoke last. That is a\n"
        "                         duplicate address, and comparing this table\n"
        "                         with the other machine's is how it is found.\n"
        "\n"
        "THE IFACE COLUMN is the card the entry was learned on, recorded by the\n"
        "kernel when it learned it -- not worked out from the mask afterwards.\n"
        "On a box with more than one card, an address answering on the wrong\n"
        "one is a cable in the wrong port, and nothing else here says so.\n"
        "\n"
        "arp -d IS A REAL DELETE. The entry goes out of the running cache and\n"
        "the next packet to that address asks again. That is the repair after a\n"
        "machine is swapped and the old MAC is still being used. If there was\n"
        "no such entry it says so and exits non-zero: it never reports a\n"
        "success it did not have.\n"
        "\n"
        "The name column is `?` in the -a form because nothing on this network\n"
        "resolves an address backwards. There is no PTR zone to ask.\n", 0644, NULL },
      { "/usr/share/man/traceroute",
        "traceroute(8)\n\n"
        "  traceroute <address or name>      twelve hops, always\n"
        "\n"
        "The real mechanism: a probe with a TTL of 1, which the first router\n"
        "decrements to zero and returns an ICMP time-exceeded for, then 2, and\n"
        "so on. Each line is the source address of the error that came back.\n"
        "The stack produces those errors for anybody's packets; this program\n"
        "only counts.\n"
        "\n"
        "  1  10.0.2.2         a router answered\n"
        "  2  *                that hop sent nothing back\n"
        "\n"
        "WHY IT IS WORTH TYPING when ping already failed: ping says it did not\n"
        "work, this says HOW FAR it got. In a building with a router per floor\n"
        "the last address that answered is the last place that is working, and\n"
        "the next one up is where to walk.\n"
        "\n"
        "NO TIMES, and that is deliberate. The stack counts a probe in whole\n"
        "milliseconds of wire time and does not hand a per-hop round trip back\n"
        "to a program. Three columns of `1 ms` would look like a measurement\n"
        "and be a constant. ping(1) measures, and prints what it measured.\n"
        "\n"
        "There is no -m, -n, -q or -w: the probe count and the hop limit belong\n"
        "to the kernel here, not to this program, and a flag nobody honoured\n"
        "would look like a setting. A name is resolved first and separately, so\n"
        "`cannot resolve` is never reported as `the path is broken`.\n", 0644, NULL },
      { "/usr/share/man/tcpdump",
        "tcpdump(8)\n\n"
        "  tcpdump --capture on           start the ring (and clear it)\n"
        "  tcpdump --capture off          stop it\n"
        "  tcpdump [-i iface] [-c n] [-Q in|out] [filter ...]\n"
        "\n"
        "  filters: arp  icmp  tcp  udp  ip  host <addr>  port <n>\n"
        "           `and` between them. Every one must match.\n"
        "\n"
        "One line per frame that really crossed this machine's card, in both\n"
        "directions, with the fields read out of the headers that were on the\n"
        "wire: the macs, the ethertype, the protocol, the addresses, the ports,\n"
        "the TCP flags, the ICMP type, the length. Every filter compares one of\n"
        "those fields -- none of them searches the text of a line.\n"
        "\n"
        "  1421 eth0 Out arp who-has 10.0.2.2 tell 10.0.2.15, length 60\n"
        "  1423 eth0 In  IP 10.0.2.2 > 10.0.2.15: icmp echo-reply, length 60\n"
        "  1450 eth0 Out IP 10.0.2.15.41001 > 10.0.2.20.80: tcp [S], length 60\n"
        "\n"
        "The leading number is the stack's clock in milliseconds of wire time.\n"
        "There is no wall clock down here, so there is no timestamp of the kind\n"
        "the real program prints.\n"
        "\n"
        "IT IS NOT LIVE. Nothing runs while your shell waits, so there is\n"
        "nobody to print a frame to as it arrives. The capture is a ring the\n"
        "stack fills as frames pass and this reads it back:\n"
        "\n"
        "  tcpdump --capture on\n"
        "  ping 10.0.2.2\n"
        "  tcpdump icmp\n"
        "\n"
        "It holds the last 256 frames and it is off until asked, because a ring\n"
        "nobody reads is memory nobody is paying for.\n"
        "\n"
        "IT SEES ONE MACHINE: what this card sent and what this card accepted.\n"
        "There is no promiscuous mode on this network -- a frame addressed to\n"
        "somebody else was never handed up here. To see the other half of a\n"
        "conversation, run it on the other box too. That is the real technique.\n"
        "\n"
        "WHAT IT REFUSES, BY NAME rather than by ignoring it: -w, -r, -X, -e,\n"
        "-v, and the pcap syntax it does not have -- src, dst, net, portrange,\n"
        "vlan, `or`, `not`. A filter that was silently dropped would turn `no\n"
        "packets matched` into evidence of something that never happened. There\n"
        "is no file to write and no hex dump: the kernel keeps the fields, not\n"
        "the bytes.\n"
        "\n"
        "netstat -w is the other capture and a different thing: the whole\n"
        "network's events, every node's, one line each. This is one card's\n"
        "frames.\n", 0644, NULL },
      { "/usr/share/man/ss",
        "ss(8)\n\n"
        "  ss            every socket this machine holds\n"
        "  ss -l         only the ones that are listening\n"
        "  ss -t         tcp only\n"
        "  ss -u         udp only\n"
        "  ss -a  -n     accepted: every socket, and numeric, which it always is\n"
        "\n"
        "SAY WHAT THIS IS: it is netstat's socket list in ss's columns. It asks\n"
        "the kernel the same question netstat does -- the real socket table --\n"
        "and there is no second source for it to read. It exists because `ss\n"
        "-ltn` is what a person types now, and a machine that answers `command\n"
        "not found` to the standard question teaches you to distrust the\n"
        "answers it does give.\n"
        "\n"
        "THERE IS NO -p. On a real system that column comes from the process\n"
        "holding the file descriptor. This kernel binds a socket for a SERVICE\n"
        "and does not record a pid against it, so the column would always be\n"
        "empty -- which reads as `nobody owns this`, which is false. `svc\n"
        "status <name>` and `ps` are the questions that have answers.\n"
        "\n"
        "A socket here is real: a service that has died has no line, and a\n"
        "service running with a configuration it never reloaded is listening on\n"
        "the port it actually loaded, not the one the file now says.\n", 0644, NULL },
      { "/usr/share/man/voice",
        "voice(8)\n\n"
        "  voice         the calls this machine has finished, and the verdict\n"
        "                on the worst of them\n"
        "  voice -l      the calls in progress at this instant\n"
        "\n"
        "THE ONE PROGRAM ON THIS MACHINE THAT READS THE PAST. netstat, ss, ip,\n"
        "arp and tcpdump all take a reading NOW: the sockets now, the\n"
        "neighbours now, the card's counters now. A call is over by the time\n"
        "anybody sits down to ask why it was bad -- the busy period ended, the\n"
        "streams were hung up with it, and `ss` shows nothing at all. So the\n"
        "stack keeps the finished calls on the node, beside the interface\n"
        "counters and for the same reason, and this reads them back.\n"
        "\n"
        "WHY A HEALTHY DESK IS NOT EVIDENCE. A phone call is 172 bytes every\n"
        "20 ms, a fiftieth of what one office desk pulls, so it is never short\n"
        "of bandwidth. It is ruined by a packet lost, a packet late, or a path\n"
        "long -- and none of those happen on this card. They happen on a port\n"
        "further up, which is why `netstat -P` here can read tx 21898 rx 21764\n"
        "drop 0 on a day every call on this desk broke up. Both numbers are\n"
        "true. Only one of them is the answer.\n"
        "\n"
        "  dir  calls    sent arrived   lost   late concealed\n"
        "  out      1    1250    1250      0    368      368  29.4%\n"
        "  in       1    1250    1250      0      0        0   0.0%\n"
        "\n"
        "  in   is audio this machine RECEIVED, and it timed every packet as\n"
        "       it landed.\n"
        "  out  is audio it SENT, as the FAR END reported hearing it. No\n"
        "       endpoint can know that on its own; a real one is told, in RTCP\n"
        "       receiver reports and the VoIP metrics block that carries\n"
        "       concealment. Nothing puts RTCP on this wire -- the report is\n"
        "       handed over when the call ends -- and that is a shortcut in\n"
        "       how the number travels, not in the number. Every packet it\n"
        "       counts really crossed a port and was really dropped or really\n"
        "       held.\n"
        "\n"
        "CONCEALED IS THE NUMBER THAT MATTERS: audio frames with no sound to\n"
        "play, because the packet was lost or because it arrived after the\n"
        "60 ms de-jitter buffer had already played the silence where it should\n"
        "have gone. It is a count of 20 ms frames, not a score anybody scaled.\n"
        "Under one per cent and nobody hears it; past five and the call is\n"
        "being given up on.\n"
        "\n"
        "AND IT NAMES THE PORT when the stack knows which one it was. A frame\n"
        "carries the stream it belongs to, so the port that dropped it or held\n"
        "it in a queue is recorded on the call rather than guessed at\n"
        "afterwards from whichever port was busiest:\n"
        "\n"
        "  verdict: unusable -- 368 packets arrived too late to play: the\n"
        "  queue on edge port 0 held them for 98.0ms, past the 60ms the\n"
        "  receiver buffers.\n"
        "\n"
        "IT SAYS SO WHEN THE CALLS WERE FINE. `verdict: clear` is an answer\n"
        "too, and a more useful one than silence: it means the audio left and\n"
        "arrived intact and whatever the complaint is, it is not the network\n"
        "under this desk. A tool that only speaks when something is wrong\n"
        "teaches you to ignore it when it is quiet.\n"
        "\n"
        "A RUN IS NOT A DAY. The stack has never heard of days. The record is\n"
        "cleared when a call starts at a machine that is not already on one,\n"
        "and each call is added as it ends -- so a desk that dials in the\n"
        "morning and hangs up in the evening holds exactly the last day's\n"
        "calls, and the wire-clock milliseconds on the first line say when\n"
        "they ran.\n"
        "\n"
        "THERE IS NO FLAG TO PLACE A CALL AND NONE TO CLEAR THE RECORD. The\n"
        "phone system dials; there is nothing here to dial with. And the\n"
        "record is usually the only copy of the evidence, so it is not one\n"
        "keystroke from being destroyed.\n", 0644, NULL },
      { "/usr/share/man/nft",
        "nft(8)\n\n"
        "  /usr/sbin/nft      reads /etc/nftables.conf at startup and LOADS it\n"
        "\n"
        "The rules are real: a packet that matches a drop is discarded on the\n"
        "way in, before anything above IP sees it, and `netstat -F` prints the\n"
        "running ruleset with what each rule has actually dropped. The file on\n"
        "disk is what the machine is SUPPOSED to do; that is what it is doing.\n"
        "The two drift the moment somebody edits the file and does not reload\n"
        "it -- `svc reload nftables`, or `kill -HUP`.\n"
        "\n"
        "WHAT IT PARSES. A small subset, and this is all of it:\n"
        "\n"
        "  policy drop            what happens to a packet no rule matched\n"
        "  policy accept\n"
        "  tcp dport { 22, 80 } accept      a set of ports\n"
        "  tcp dport 8080 drop              one port\n"
        "  udp dport 53 accept\n"
        "  icmp accept                      ping, and the errors that carry a\n"
        "                                   diagnosis: unreachable, time\n"
        "                                   exceeded\n"
        "  icmp drop\n"
        "  ip protocol icmp accept          the same rule, spelled in full\n"
        "  tcp accept                       every port of one protocol\n"
        "\n"
        "The first rule that matches decides, in file order, and the policy is\n"
        "applied last because that is what a policy is. A rule with no verdict\n"
        "is skipped rather than guessed at.\n"
        "\n"
        "THE ONE THIS MACHINE SHIPS accepts tcp 22 and 80 and drops everything\n"
        "else, and there is no connection tracking for anything but a socket\n"
        "this machine already holds. So a pristine box will not answer a ping,\n"
        "and that is not a fault: it is the ruleset, doing what it says. The\n"
        "repair a real administrator makes is a line, not a policy:\n"
        "\n"
        "  sed -i \"s/tcp dport/icmp accept\\\\n    tcp dport/\" /etc/nftables.conf\n"
        "  svc reload nftables\n"
        "  netstat -F\n"
        "\n"
        "Turning the whole policy to accept works too, and opens every port on\n"
        "the machine to do it.\n", 0644, NULL },
      { "/usr/share/man/ed",
        "ed(1)\n\n"
        "  ed <file>                    open it and say how big it is\n"
        "  ed <file> <command> ...      each argument is one line of ed input\n"
        "\n"
        "THE LINE EDITOR. It is the editor every repair on this machine ends\n"
        "with: `man ip` and `netstat -F` and the shell's own `help` all say to\n"
        "edit a file, and this is what edits it.\n"
        "\n"
        "IT IS NOT INTERACTIVE, AND IT CANNOT BE. A program here runs to\n"
        "completion inside one command -- nothing more will be typed until it\n"
        "has exited -- so there is nowhere for it to stop and ask you for the\n"
        "next line. The session is the argument list instead: each argument is\n"
        "one line you would have typed at an ed prompt, and the commands and\n"
        "the addressing are ed's.\n"
        "\n"
        "COMMANDS\n"
        "  p      print the addressed lines\n"
        "  n      print them with their numbers\n"
        "  =      the current line's number\n"
        "  d      delete the addressed lines\n"
        "  a      add text after the addressed line\n"
        "  i      add text before it\n"
        "  c      replace the addressed lines with text\n"
        "  s      substitute -- s/old/new/, and /g for every match on a line\n"
        "  w      write the buffer back. `w /tmp/other` writes elsewhere\n"
        "  q      stop reading commands\n"
        "\n"
        "ADDRESSES go in front of the command. A line number, `.` for the\n"
        "current line, `$` for the last, `a,b` for a range, and `,` on its own\n"
        "for the whole buffer. The default is the current line, which starts\n"
        "at the end of the file, as ed's does. Address 0 means before the\n"
        "first line, so `0i` and `0a` both put one at the top.\n"
        "\n"
        "TEXT FOR a, i AND c is the arguments that follow, ended by a lone\n"
        "`.` -- the same full stop that ends it at an ed prompt. Leaving it\n"
        "out is refused by name rather than guessed at.\n"
        "\n"
        "NOTHING IS WRITTEN UNTIL YOU SAY `w`. If the buffer changed and the\n"
        "script had no `w` in it, ed says the file is NOT saved and exits\n"
        "non-zero: a change you did not write is a change that did not happen,\n"
        "and finding that out from a service that stayed down is worse.\n"
        "\n"
        "SUBSTITUTION IS PLAIN TEXT, not a regular expression -- `.` is a full\n"
        "stop and matches only a full stop. Any delimiter works, which is how\n"
        "a path is typed:  s|/usr/local|/opt|  and \\\\t and \\\\ are understood.\n"
        "An s that matches nothing is an error and changes no line, because a\n"
        "substitution that quietly did nothing is how a player comes to\n"
        "believe a file was repaired when it was not.\n"
        "\n"
        "WORKED EXAMPLE -- the resolver, which is the repair man ip and man\n"
        "netd both describe:\n"
        "\n"
        "  ed /etc/resolv.conf ,n\n"
        "  ed /etc/resolv.conf 1c \"nameserver 10.0.2.3\" . w\n"
        "  svc reload net\n"
        "\n"
        "and a line added at the end of a file, and one taken out:\n"
        "\n"
        "  ed /etc/hosts '$a' \"10.0.2.9  build\" . w\n"
        "  ed /etc/fstab 4d w\n"
        "\n"
        "LIMITS, because there is no allocator on this machine: 4096 lines and\n"
        "64 KB of file. `sed -i` streams and has neither, so it is still the\n"
        "right tool on /var/log/messages. ed is the right one on a config.\n"
        "\n"
        "There is no undo. `ed <file> ,n` first: it costs nothing and it is\n"
        "the only way to know what the line numbers are.\n", 0644, NULL },
      { "/usr/share/man/rev",
        "rev(1)\n\n"
        "  rev [file ...]     reverse each line; with no file, stdin\n"
        "\n"
        "The trailing newline stays where it is, and a stray carriage return\n"
        "at the end of a line is dropped rather than reversed to the front of\n"
        "it.\n"
        "\n"
        "It is its own undo, which makes it the cheapest test that a pipeline\n"
        "on this machine really carries bytes rather than pretending to:\n"
        "\n"
        "  cat /etc/hostname | rev | rev\n"
        "\n"
        "If that is not `cat /etc/hostname` then the pipe is what is wrong.\n",
        0644, NULL },
    }, 26
};

/* THE JOKE PACKAGE, built exactly like the serious ones.
 *
 * It is here for two reasons beyond being funny. The first is that a machine
 * with nothing pointless on it does not feel like a machine anyone used; every
 * box that has ever had an administrator has a cow on it somewhere. The second
 * is that these are ORDINARY packages and ORDINARY binaries -- `pkg owns
 * /usr/bin/sl` answers, `ldd /usr/bin/cowsay` lists libc, a bad libc kills the
 * train along with everything else -- so poking at the toys teaches exactly
 * the same tools as poking at sshd, with none of the fear.
 *
 * The fortunes are DATA, in a file, not strings inside the binary. That is the
 * difference between something you can `cat`, `grep`, `wc`, damage, verify and
 * repair, and something you can only run. */
static const Package PKG_FUN = {
    "nomfun", "1.4", "the fortune cookie, the cow and the train",
    {
      { "/usr/bin/fortune", NULL, 0755, NULL },
      { "/usr/bin/cowsay",  NULL, 0755, NULL },
      { "/usr/bin/sl",      NULL, 0755, NULL },
      /* rot13 is in the joke package and is not a joke. It is here because
       * somebody on this machine used it for exactly what people really use
       * it for: stopping a sentence being read by ACCIDENT by whoever walks
       * past. Running it twice is the undo, which is the entire security
       * model and the man page says so. */
      { "/usr/bin/rot13",   NULL, 0755, NULL },
      /* One per line, because `fortune` reads a line and because that makes
       * the file greppable. Blank lines and #comments are skipped, and an
       * indented line continues the one above it -- which is the same shape
       * ~nomowner's own fortunes file already had. */
      { "/usr/share/fortunes",
        "# /usr/share/fortunes -- read by /usr/bin/fortune, one per line.\n"
        "# An indented line continues the one above it. Comments and blank\n"
        "# lines are skipped. Add your own; nothing here is compiled in.\n"
        "Backups are a theory. Restores are a fact.\n"
        "There is no cloud. There is only somebody else's /dev/sda1.\n"
        "ls sees the arrow. stat follows it. Only one of them is your friend.\n"
        "The machine is always right. The machine is describing what you did.\n"
        "Any sufficiently enthusiastic cleanup script is an attacker with a\n"
        "  crontab entry.\n"
        "It is not a bug, it is an undocumented feature of the initrd.\n"
        "df first. It is free, it takes two seconds, and it has been the\n"
        "  answer twice.\n"
        "A service can be running and still wrong. Ask /run for what it loaded.\n"
        "\"Nothing changed\" means nothing changed that they wish to discuss.\n"
        "The uptime record and the patch level are the same conversation.\n"
        "Nobody wants a backup. Everybody wants a restore.\n"
        "pkg verify is clean and it still will not boot: the boot sector is\n"
        "  not a file.\n"
        "Every well-formed UUID belongs to some disk. Not necessarily this one.\n"
        "To err is human. To blame the previous administrator is systems\n"
        "  administration.\n"
        "The fastest way to learn who owns a file is pkg owns. The second\n"
        "  fastest is to delete it.\n"
        "Reinstalling a package you have not diffed is how you get two problems.\n"
        "Read the console before the wiki. The console was there.\n"
        "A reboot is a diagnostic, not a repair. It only destroys the evidence\n"
        "  faster.\n"
        "Log rotation is a chore right up until the morning it is an outage.\n"
        "The correct number of critical services a monitoring agent may add to\n"
        "  your boot is zero.\n"
        "Everything broken means libc. Two things broken means asking what\n"
        "  those two have in common.\n"
        "ldd never disagrees with the loader. People do.\n"
        "Documentation is a love letter you write to yourself at four in the\n"
        "  morning.\n"
        "The severity of an outage is proportional to how recently somebody\n"
        "  said \"it's fine\".\n"
        "Never trust a config you have not cat'ed today.\n"
        "There are two hard problems in this job: naming things, cache\n"
        "  invalidation, and off-by-one errors.\n"
        "A mode of 000 is not a mystery. It is a person, and they will do it\n"
        "  again.\n"
        "The disk is never full of anything interesting.\n"
        "df -i exists for the one day a year when it is the only answer.\n"
        "Do not rm in anger. rm does not care and cannot be argued with.\n"
        "A migration is complete when the old machine is switched off, and not\n"
        "  one day before.\n"
        "Every temporary mount outlives the person who made it.\n"
        "If you did not write it down it did not happen, and if you wrote it in\n"
        "  /tmp, ask yourself who cleans /tmp.\n"
        "Nine tickets in ten are one word in one line of one file.\n"
        "It is always DNS, except here, where it is /etc/hosts and you typed it\n"
        "  yourself.\n"
        "A machine that boots is not the same thing as a machine that is well.\n"
        "The bootloader has one job and reads one file. Read the same file.\n"
        "Somebody's local edit is somebody's afternoon. Do not --force without\n"
        "  looking.\n"
        "Yes, it worked in testing. Testing is where the wrong libc lives.\n"
        "Two administrators, one /etc, no agreement. This is why ns exists.\n"
        "The train has cost me more hours than any outage and I regret nothing.\n"
        "Root is not a skill level.\n"
        "The last person who touched it is not the person who broke it, but\n"
        "  they are the person you can still telephone.\n", 0644, NULL },
      { "/usr/share/man/fortune",
        "fortune(6)\n\n"
        "  fortune            one line from /usr/share/fortunes\n"
        "  fortune FILE       one line from FILE\n"
        "\n"
        "One fortune per line; an indented line continues the one above, and\n"
        "#comments and blank lines are skipped.\n"
        "\n"
        "The quotes are a file, not part of the program: cat, grep and wc all\n"
        "work on /usr/share/fortunes, and pkg verify nomfun notices when it has\n"
        "been damaged. Try `fortune /home/nomowner/fortunes` for the previous\n"
        "administrator's own list.\n"
        "\n"
        "There is no clock on this machine, so the choice is made from the\n"
        "process id. Two runs in a row give different lines; the same pid would\n"
        "give the same line, which nothing here can arrange.\n", 0644, NULL },
      { "/usr/share/man/cowsay",
        "cowsay(6)\n\n"
        "  cowsay <words>     a cow says it\n"
        "  cowsay -f <face>   cow (default), tux, dragon, daemon\n"
        "  ... | cowsay       with no words it reads stdin, which is the point\n"
        "\n"
        "  fortune | cowsay -f tux\n"
        "\n"
        "The balloon is measured: the text wraps at 40 columns and the box is\n"
        "as wide as the longest line that came out of the wrap.\n", 0644, NULL },
      { "/usr/share/man/sl",
        "sl(6)\n\n"
        "  sl                 a steam locomotive, because you meant ls\n"
        "\n"
        "It does not animate. Nothing on this machine redraws the screen, and a\n"
        "program that pretended to would be the only dishonest thing in\n"
        "/usr/bin, so the train is drawn all at once with its smoke behind it.\n", 0644, NULL },
      { "/usr/share/man/rot13",
        "rot13(1)\n\n"
        "  rot13 [file ...]   rotate letters by thirteen; with no file, stdin\n"
        "\n"
        "Letters move thirteen places and wrap. Digits, punctuation and\n"
        "spacing are untouched, so a file that has been through rot13 keeps\n"
        "its shape -- which is how you can tell rot13 from damage at a glance:\n"
        "the line lengths are right and the words are the right lengths and\n"
        "none of it is words.\n"
        "\n"
        "Thirteen is half of twenty-six, so it is its own undo:\n"
        "\n"
        "  rot13 somefile | rot13\n"
        "\n"
        "gives the file back. THIS IS NOT ENCRYPTION and nothing about it is\n"
        "private: anybody who can run rot13 can undo rot13. It is for keeping\n"
        "a punchline, a spoiler or an opinion from being read by ACCIDENT over\n"
        "somebody's shoulder, which is a real thing people want and is the\n"
        "only thing this offers.\n"
        "\n"
        "`ls` does not list a name that begins with a dot. `ls -a` does, `find`\n"
        "does -- `find <dir> -name \".*\"` finds nothing else -- and `du` counts\n"
        "them into its total whether you can see them or not. Worth remembering\n"
        "the next time a directory looks emptier than the disk says it is.\n", 0644, NULL },
      { "/usr/share/doc/nomfun/README",
        "nomfun 1.4 -- the fortune cookie, the cow, the train, and rot13.\n"
        "\n"
        "  /usr/bin/fortune      one line from /usr/share/fortunes\n"
        "  /usr/bin/cowsay       a cow says it. -f cow, tux, dragon, daemon\n"
        "  /usr/bin/sl           you meant ls\n"
        "  /usr/bin/rot13        thirteen places, its own undo\n"
        "  /usr/share/fortunes   THE QUOTES, in a file\n"
        "\n"
        "This package exists for two reasons beyond being funny.\n"
        "\n"
        "The first is that a machine with nothing pointless on it does not feel like\n"
        "a machine anybody ever used. Every box that has had an administrator has a\n"
        "cow on it somewhere.\n"
        "\n"
        "The second is the useful one. These are ORDINARY packages containing\n"
        "ORDINARY binaries. `pkg owns /usr/bin/sl` answers. `ldd /usr/bin/cowsay`\n"
        "lists libc. A bad libc kills the train along with everything else, and a\n"
        "`chmod 000` sweep across /usr/bin disarms the cow exactly as it disarms\n"
        "sshd. So you can learn every tool in this game by poking at the toys, with\n"
        "none of the fear, and then use the same tools on something that matters.\n"
        "\n"
        "THE FORTUNES ARE DATA. They are in /usr/share/fortunes, not inside the\n"
        "binary, which is the difference between something you can cat, grep, wc,\n"
        "damage, verify and repair and something you can only run.\n"
        "\n"
        "  fortune | cowsay -f tux\n"
        "  wc /usr/share/fortunes\n"
        "  pkg verify nomfun\n"
        "  fortune /home/nomowner/fortunes\n", 0644, NULL },
      { "/usr/share/doc/nomfun/CHANGELOG",
        "nomfun CHANGELOG -- newest first.\n"
        "\n"
        "1.4  -- current. rot13, with a man page that refuses to call it encryption.\n"
        "       It is here because somebody on this machine used it for the thing\n"
        "       people really use it for, which is not being read by accident.\n"
        "\n"
        "1.3  -- fortune reads a FILE rather than a compiled-in array, so the quotes\n"
        "       became something you can grep and something `pkg verify` notices when\n"
        "       it is damaged. An indented line continues the one above it.\n"
        "\n"
        "1.2  -- cowsay measures its balloon: the text wraps at 40 columns and the\n"
        "       box is as wide as the longest line the wrap produced. With no words\n"
        "       it reads stdin, which is the point of it.\n"
        "\n"
        "1.1  -- sl. It does not animate, and it never will: nothing on this machine\n"
        "       redraws the screen, and a program that pretended to would be the only\n"
        "       dishonest thing in /usr/bin.\n"
        "\n"
        "1.0  -- fortune.\n", 0644, NULL },
    }, 11
};

static const Package PKG_MAIL = {
    "postfix", "3.8", "mail transport",
    {
      { "/usr/sbin/postfix", NULL, 0755, NULL },
      { "/etc/postfix/main.cf",
        "myhostname = nominal.local\nrelayhost = 10.0.2.30\n", 0644, NULL },
      { "/etc/aliases", "root: nomowner\npostmaster: root\n", 0644, NULL },
      { "/etc/services.d/postfix.svc",
        "# /etc/services.d/postfix.svc\n"
        "name: postfix\nexec: /usr/sbin/postfix\n"
        "description: mail transport\nafter: net\n"
        "restart: on-failure\nenabled: no\nrunlevel: 3\n", 0644, NULL },
    }, 4
};

static const Package PKG_ACCT = {
    "acct", "6.6", "process accounting",
    {
      { "/usr/sbin/accton", "#!accton\n", 0755, NULL },
      { "/etc/default/acct", "ACCT_ENABLE=no\n", 0644, NULL },
    }, 2
};

static const Package PKG_TZ = {
    "tzdata", "2024a", "time zones",
    {
      { "/etc/timezone", "UTC\n", 0644, NULL },
      { "/usr/share/zoneinfo/UTC", "UTC0\n", 0644, NULL },
    }, 2
};

static const Package PKG_TERMINFO = {
    "ncurses", "6.4", "terminal handling",
    {
      { "/lib/libncurses.so.6", "\x7fELF (stub) ncurses 6.4\n", 0755, NULL },
      { "/usr/share/terminfo/vt100", "vt100|dec vt100\n", 0644, NULL },
      { "/usr/share/terminfo/linux", "linux|linux console\n", 0644, NULL },
    }, 3
};

static const Package PKG_AUDIT = {
    "audit", "3.1", "the audit trail",
    {
      { "/usr/sbin/auditd", NULL, 0755, NULL },
      { "/etc/audit/auditd.conf", "log_file = /var/log/audit.log\nmax_log_file = 8\n", 0644, NULL },
      { "/etc/services.d/audit.svc",
        "# /etc/services.d/audit.svc\n"
        "name: audit\nexec: /usr/sbin/auditd\n"
        "description: audit trail\nafter: syslog\n"
        "restart: on-failure\nenabled: yes\nrunlevel: 3 5\n", 0644, NULL },
    }, 3
};

static const Package *IMAGE[] = {
    &PKG_BASE, &PKG_USERS, &PKG_BOOTLOADER, &PKG_KERNEL, &PKG_SYSINIT,
    &PKG_SHELL, &PKG_UDEV, &PKG_SYSLOG, &PKG_NET, &PKG_SSH, &PKG_HAMDE,
    &PKG_SOUNDS,
    &PKG_HOME, &PKG_PKGCONF, &PKG_LIBC, &PKG_ZLIB, &PKG_CRON, &PKG_LOGROTATE, &PKG_NTP,
    &PKG_HTTPD, &PKG_FIREWALL, &PKG_MAN, &PKG_MAIL, &PKG_ACCT, &PKG_TZ,
    &PKG_TERMINFO, &PKG_AUDIT, &PKG_FUN,
};
#define IMAGE_N ((int)(sizeof IMAGE / sizeof IMAGE[0]))

void image_generated(const Machine *m, const char *path, Buf *out);
static void install_local_edits(Machine *m, uint64_t seed);
static void install_history(Machine *m);

/* ---------------------------------------------------------- the rescue --
 * A complete, separate system on its own medium. It is never corrupted: the
 * breaker only ever touches m->disk. That is what makes it a live image and
 * not just another thing that can go wrong.
 *
 * It is package-backed like everything else, so `pkg verify` works inside it
 * too -- and so a player who has learned the rescue system has learned the
 * customer's, because they are the same system with different contents.
 */
static const Package PKG_RESCUE_BASE = {
    "rescue-base", "3.2", "the live rescue system",
    {
      { "/etc/inittab",
        "# rescue medium: straight to a shell.\n"
        "/bin/rc /etc/rc.boot\n", 0644, NULL },
      /* Literal, not generated. This used to be NULL and image_generated()
       * decided what to put here by looking at m->on_rescue -- so
       * `pkg reinstall sysinit` while booted from the rescue medium wrote the
       * RESCUE's rc.boot onto the CUSTOMER's disk, every single time. Generated
       * content must never depend on mutable machine state. */
      { "/etc/rc.boot",
        "# /etc/rc.boot on the rescue medium.\n"
        "echo rescue: live system, read-only medium\n"
        "echo rescue: the customer disk is /dev/sda1 and is NOT mounted\n"
        "echo\n"
        /* AND SAY WHAT THAT MEANS FOR THE THING YOU CAME HERE TO DO. The
         * medium refuses writes now -- it always claimed to and used to
         * accept them -- so the next thing a player tries after reading this
         * banner is an edit that gets turned down. Naming the trap here is
         * cheaper than leaving them to work out that /etc on this shell is
         * the disc's /etc and not the customer's. */
        "echo   NOTHING HERE TAKES WRITES. /etc on this shell is the disc's\n"
        "echo   own, not the customer's, and editing it is refused. The disk\n"
        "echo   is reached by mounting it, and /mnt/etc IS the customer's:\n"
        "echo\n"
        "echo   mount /dev/sda1 /mnt\n"
        "echo   for i in dev sys proc; do mount /$i /mnt/$i; done\n"
        "echo   chroot /mnt\n"
        "echo\n"
        "echo   links wiki.nomnix.org/rescue   for the full procedure\n"
        "echo\n", 0755, NULL },
      { "/etc/hostname", "rescue\n", 0644, NULL },
      { "/etc/issue",    "NomnixOS rescue 3.2 -- live medium\n", 0644, NULL },
      { "/etc/os-release",
        "NAME=\"NomnixOS Rescue\"\nVERSION=\"3.2\"\nID=nomnix-rescue\n", 0644, NULL },
      { "/etc/fstab", "# nothing is mounted automatically on the rescue medium\n",
        0644, NULL },
      { "/etc/hosts",
        "127.0.0.1       localhost\n"
        "10.0.2.20       wiki.nomnix.org wiki\n"
        "10.0.2.30       support.internal support\n"
        "10.0.2.44       bofh.nomnix.org bofh\n", 0644, NULL },
      { "/etc/resolv.conf", "nameserver 10.0.2.3\n", 0644, NULL },
      /* The live medium has its OWN libc. That is the whole point of it: when
       * the customer's libc is wrong, nothing on their disk runs, including
       * the tools you would fix it with. */
      { "/lib/libc.so.6",  "stub libc 2.38\n", 0755, NULL },
      { "/lib/libm.so.6",  "stub libm 2.38\n", 0755, NULL },
      { "/etc/ld.so.conf", "/lib\n/usr/lib\n", 0644, NULL },
      { "/etc/motd",
        "NomnixOS rescue medium.\n"
        "  the customer disk is /dev/sda1 and is not mounted\n"
        "  links wiki.nomnix.org/rescue    for the procedure\n", 0644, NULL },
      { "/usr/lib/sysinit/init", NULL, 0755, NULL },
      { "/sbin/init", NULL, 0777, "/usr/lib/sysinit/init" },
      /* THE OTHER HIDDEN THING, and reaching it is a real step: you have to
       * boot the live medium and then look somewhere on it that has nothing
       * to do with repairing anything. Nobody browses a rescue disc.
       *
       * It pays out in technique that is true and is written nowhere else:
       * that `pkg verify` on the rescue medium cheerfully verifies THE
       * RESCUE MEDIUM and says the reassuring sentence about the wrong
       * machine; that --root is not chroot and why that matters on the day
       * the libc is the casualty; that ldd says out loud which root it
       * resolved against. Every line of it was checked by running it. */
      { "/root/burn-notes.txt",
        "Notes from whoever burned this disc. If you are reading this you went\n"
        "looking in /root on a rescue medium, which nobody does, so these are the\n"
        "things I only ever tell people who ask.\n"
        "\n"
        "1. THIS MEDIUM IS NEVER DAMAGED.\n"
        "\n"
        "   The customer's disk is /dev/sda1 and it is where the fault is. This is\n"
        "   /dev/sr0 and the breaker never touches it. That makes it the only fixed\n"
        "   point you have: when a command here behaves oddly, it is not the disc.\n"
        "   It is the command, or the arguments, or you. I have watched people lose\n"
        "   an hour to doubting the one thing in the room that cannot be wrong.\n"
        "\n"
        "2. pkg VERIFIES WHATEVER ROOT YOU POINT IT AT, AND BY DEFAULT THAT IS THIS\n"
        "   ONE.\n"
        "\n"
        "     pkg verify                 verifies THE RESCUE MEDIUM. It will say\n"
        "                                \"all files match their packages\" and it will\n"
        "                                be telling the truth about the wrong machine.\n"
        "     pkg --root /mnt verify     verifies the customer's disk.\n"
        "\n"
        "   That one is worth reading twice, because the reassuring answer and the\n"
        "   useless answer are the same sentence.\n"
        "\n"
        "3. --root IS NOT chroot, AND THE DIFFERENCE IS THE WHOLE POINT.\n"
        "\n"
        "   chroot /mnt runs THE CUSTOMER'S programs. When their libc is the\n"
        "   casualty, nothing on that disk runs at all, so chroot cannot help you --\n"
        "   the shell you would land in is one of the things that will not start.\n"
        "\n"
        "   `pkg --root /mnt` runs THIS medium's pkg against THEIR files. It works on\n"
        "   a disk with nothing runnable left on it, which is the day you need it.\n"
        "   Same for ldd:\n"
        "\n"
        "     ldd /mnt/usr/sbin/httpd\n"
        "\n"
        "   and it says \"(resolving against the root filesystem at /mnt)\", which is\n"
        "   it telling you it used their /etc/ld.so.conf and their libraries and not\n"
        "   ours. That sentence is the difference between an answer about their\n"
        "   machine and an answer about mine.\n"
        "\n"
        "4. THE FAILED BOOT LEFT A LOG AND YOU CAN READ IT FROM HERE.\n"
        "\n"
        "     dmesg -r /mnt      their boot log, read from outside their machine\n"
        "     dmesg -1           the boot before the one you are looking at\n"
        "\n"
        "   A boot that failed still wrote a log while it was failing. People power\n"
        "   cycle the box and destroy it, then ask what it said.\n"
        "\n"
        "5. WHAT I PUT ON THIS DISC THAT IS NOT STRICTLY A REPAIR TOOL.\n"
        "\n"
        "   /usr/bin/rot13, /bin/seq and /bin/rev, and only the first one needs\n"
        "   defending. Somebody I worked with kept notes in rot13 -- not secrets,\n"
        "   just things they did not want read over their shoulder -- and the day\n"
        "   their machine would not boot, those notes were the most useful files on\n"
        "   the disk and there was no way to read them. So it is on the disc now.\n"
        "\n"
        "     ls -a /mnt/home/<whoever>\n"
        "     rot13 /mnt/home/<whoever>/<whatever it turns out to be>\n"
        "\n"
        "   `ls` does not list a name that starts with a dot. It never has. A home\n"
        "   directory that looks tidy is usually a home directory you have not\n"
        "   actually looked at.\n"
        "\n"
        "6. THE ORDER, AND IT IS ON THE WIKI TOO.\n"
        "\n"
        "     rcon media insert\n"
        "     rcon boot media\n"
        "     rcon power cycle\n"
        "     mount /dev/sda1 /mnt\n"
        "     dmesg -r /mnt\n"
        "     pkg --root /mnt verify\n"
        "     blkid\n"
        "\n"
        "   `links wiki.nomnix.org/rescue` for the full version, if their network\n"
        "   is up. Ours is. This medium's /etc/hosts is its own.\n", 0644, NULL },
    }, 15
};

static const Package PKG_RESCUE_TOOLS = {
    "rescue-tools", "3.2", "the tools on the live medium",
    {
      /* The live medium carries EVERY repair tool. It had been quietly
       * missing most of them -- no grep, no sed, no cp, no mkinitrd --
       * because three separate edits that meant to add them did not
       * match, and nothing checked. A rescue disc without the tools is
       * not a rescue disc. */
      { "/bin/rc", NULL, 0755, NULL },
      { "/bin/sh", NULL, 0755, NULL },
      { "/bin/ls", NULL, 0755, NULL },
      { "/bin/cat", NULL, 0755, NULL },
      { "/bin/ps", NULL, 0755, NULL },
      { "/bin/ns", NULL, 0755, NULL },
      { "/bin/stat", NULL, 0755, NULL },
      { "/bin/chmod", NULL, 0755, NULL },
      { "/bin/mount", NULL, 0755, NULL },
      { "/bin/umount", NULL, 0755, NULL },
      { "/bin/chroot", NULL, 0755, NULL },
      /* Essential here specifically: when the disk's own libc is wrong,
       * nothing on that disk runs, so the only ldd you can use is this one --
       * pointed at the broken binary through /mnt. */
      { "/usr/bin/ldd", NULL, 0755, NULL },
      /* Essential on the live medium: the whole point is reading the log of a
       * boot that failed, from a system that did not. */
      { "/bin/dmesg", NULL, 0755, NULL },
      { "/bin/cp", NULL, 0755, NULL },
      { "/bin/mv", NULL, 0755, NULL },
      { "/bin/rm", NULL, 0755, NULL },
      { "/bin/touch", NULL, 0755, NULL },
      { "/bin/grep", NULL, 0755, NULL },
      { "/bin/sed", NULL, 0755, NULL },
      /* An editor belongs on a rescue medium more than anywhere else: the
       * repair the live disc exists for is a line in a config on a disk that
       * will not boot, and until now the only way to change one was a
       * substitution that had to match text you could not print. */
      /* THE COMPANY'S API, ON THE COMPANY'S WORKSTATION.
       *
       * The one program RUNBOOK adds to NOMINAL's userland, and the reason
       * the emulated machine is here at all (handoff decision 13): with it,
       * every verb the desktop's buttons send is one shell command away, so a
       * script on this box can do the job. Without it the machine is a very
       * elaborate text editor. See guest/rb.c. */
      { "/bin/rb", NULL, 0755, NULL },
      /* THE LANGUAGE. A Python subset -- indentation, if/elif/else, while,
       * for/in, def, integers, strings, lists and dicts -- lexed, compiled to
       * bytecode and executed BY A PROGRAM ON THIS DISK, on this CPU.
       * Decision 14 asked for Python because the audience knows it; decision
       * 13 asked for it to run here. See guest/py.c. */
      { "/bin/py", NULL, 0755, NULL },
      /* SCRIPTS THAT WORK, ON THE DISK, FROM THE FIRST MORNING.
       *
       * Handoff §15 calls M4 the hypothesis: does the relief of the first
       * script land? A player who has to invent the whole idea of automation
       * from a blank prompt mostly does not get there, and the macro recorder
       * (§16.2, still to build) is the on-ramp for the ones who never will.
       *
       * These are the other half of that ramp and they cost nothing: a
       * working script you can read, run, and then change. The comments in
       * onboard.py name the three things it does WRONG on purpose -- no
       * retry, no verification, no exceptions -- because those three are Act
       * II, and a player who finds them by reading is a player who has
       * already understood what the act is about. */
      { "/root/examples/README", NULL, 0644, NULL },
      { "/root/examples/queue.sh", NULL, 0755, NULL },
      { "/root/examples/onboard.py", NULL, 0644, NULL },
      /* DOES THE LANGUAGE ACTUALLY WORK? A player debugging their own logic
       * against an interpreter that is quietly wrong will conclude they
       * cannot program, which is the exact opposite of what this game is for.
       * So the answer is on the disk, runnable, and the machine gate runs
       * it. It doubles as the clearest documentation of what the language
       * has: everything in it is a thing you can use. */
      { "/root/examples/selftest.py", NULL, 0644, NULL },
      { "/root/examples/selftest.sh", NULL, 0755, NULL },
      { "/bin/ed", NULL, 0755, NULL },
      { "/bin/echo", NULL, 0755, NULL },
      { "/bin/wc", NULL, 0755, NULL },
      { "/bin/head", NULL, 0755, NULL },
      /* The rescue medium gets them too: reading the tail of the customer's
       * log through /mnt is exactly the job the live medium exists for. */
      { "/bin/tail", NULL, 0755, NULL },
      { "/bin/du", NULL, 0755, NULL },
      { "/bin/mkdir", NULL, 0755, NULL },
      { "/bin/uname", NULL, 0755, NULL },
      { "/bin/whoami", NULL, 0755, NULL },
      { "/bin/df", NULL, 0755, NULL },
      { "/sbin/fsck", NULL, 0755, NULL },
      { "/sbin/blkid", NULL, 0755, NULL },
      { "/bin/kill", NULL, 0755, NULL },
      { "/usr/bin/svc", NULL, 0755, NULL },
      { "/usr/bin/pkg", NULL, 0755, NULL },
      { "/usr/bin/links", NULL, 0755, NULL },
      { "/usr/bin/man", NULL, 0755, NULL },
      { "/usr/sbin/zbl-install", NULL, 0755, NULL },
      { "/usr/sbin/zbl-mkconfig", NULL, 0755, NULL },
      { "/usr/bin/mkinitrd", NULL, 0755, NULL },
      /* seq and rev because the live medium carries the tools, and rot13
       * because the day somebody's machine will not boot is the day their
       * rot13'd notes are the most useful files on the disk and there is no
       * way to read them. /root/burn-notes.txt explains itself. */
      { "/bin/seq", NULL, 0755, NULL },
      { "/bin/rev", NULL, 0755, NULL },
      { "/usr/bin/rot13", NULL, 0755, NULL },
    }, 42
};

static const Package *RESCUE_IMAGE[] = { &PKG_RESCUE_BASE, &PKG_RESCUE_TOOLS };

static void install_rescue(Machine *m)
{
    /* image_generated() asks the machine which medium it is describing, so it
     * has to be told before the rescue root is written -- otherwise the live
     * medium is installed with the customer's rc.boot on it, and the one
     * thing guaranteed to work is broken by construction. */
    bool was = m->on_rescue;
    m->on_rescue = true;
    vfs_init(&m->rescue);
    static const char *DIRS[] = {
        "/bin", "/dev", "/etc", "/mnt", "/proc", "/root", "/sbin", "/sys",
        "/tmp", "/usr", "/usr/bin", "/usr/lib", "/usr/lib/sysinit", "/usr/sbin",
        "/var", "/var/lib", "/var/lib/pkg", NULL
    };
    for (int i = 0; DIRS[i]; i++) vfs_mkdir(&m->rescue, DIRS[i]);

    /* Device nodes, so `ls /dev` on the rescue medium shows you what there is
     * to mount. The customer disk is present whether or not it works. */
    for (const char **d = (const char *[]){ "sda", "sda1", "sr0", NULL }; *d; d++) {
        char p2[NOM_PATH_MAX];
        snprintf(p2, sizeof p2, "/dev/%s", *d);
        VNode *n = vfs_mkfile(&m->rescue, p2, "");
        if (n) { n->kind = VN_DEV; n->mode = 0660; }
    }

    Vfs *save = NULL; (void)save;
    for (int i = 0; i < 2; i++) {
        const Package *p = RESCUE_IMAGE[i];
        for (int j = 0; j < p->nfiles; j++) {
            const PkgFile *f = &p->file[j];
            if (f->link) { vfs_symlink(&m->rescue, f->link, f->path); continue; }
            Buf b = {0};
            if (f->content) buf_puts(&b, f->content);
            else            image_generated(m, f->path, &b);
            VNode *n = vfs_mkfile(&m->rescue, f->path, "");
            if (n) {
                buf_clear(&n->data);
                buf_put(&n->data, b.p, b.len);
                n->mode = f->mode;
            }
            buf_free(&b);
        }
    }
    m->on_rescue = was;
}

/* Content that belongs to a package but names THIS installation, plus the
 * userland sources, which live in C string literals but are files on the disk
 * in every sense that matters: they are read, compiled and executed from
 * there, and `pkg reinstall` restores them from here. */
void image_generated(const Machine *m, const char *path, Buf *out)
{
    if (strcmp(path, "/usr/lib/sysinit/init") == 0)
        buf_put(out, (const char *)GUEST_INIT, GUEST_INIT_LEN);
    else if (strcmp(path, "/bin/rc") == 0)
        buf_put(out, (const char *)GUEST_RC, GUEST_RC_LEN);
    else if (strcmp(path, "/bin/sh") == 0)
        buf_put(out, (const char *)GUEST_SH, GUEST_SH_LEN);
    else if (strcmp(path, "/bin/ls") == 0)
        buf_put(out, (const char *)GUEST_LS, GUEST_LS_LEN);
    else if (strcmp(path, "/bin/cat") == 0)
        buf_put(out, (const char *)GUEST_CAT, GUEST_CAT_LEN);
    else if (strcmp(path, "/bin/ps") == 0)
        buf_put(out, (const char *)GUEST_PS, GUEST_PS_LEN);
    else if (strcmp(path, "/bin/ns") == 0)
        buf_put(out, (const char *)GUEST_NS, GUEST_NS_LEN);
    else if (strcmp(path, "/bin/stat") == 0)
        buf_put(out, (const char *)GUEST_STAT, GUEST_STAT_LEN);
    else if (strcmp(path, "/bin/chmod") == 0)
        buf_put(out, (const char *)GUEST_CHMOD, GUEST_CHMOD_LEN);
    else if (strcmp(path, "/bin/mount") == 0)
        buf_put(out, (const char *)GUEST_MOUNT, GUEST_MOUNT_LEN);
    else if (strcmp(path, "/bin/umount") == 0)
        buf_put(out, (const char *)GUEST_UMOUNT, GUEST_UMOUNT_LEN);
    else if (strcmp(path, "/bin/chroot") == 0)
        buf_put(out, (const char *)GUEST_CHROOT, GUEST_CHROOT_LEN);
    else if (strcmp(path, "/bin/cp") == 0)
        buf_put(out, (const char *)GUEST_CP, GUEST_CP_LEN);
    else if (strcmp(path, "/bin/mv") == 0)
        buf_put(out, (const char *)GUEST_MV, GUEST_MV_LEN);
    else if (strcmp(path, "/bin/rm") == 0)
        buf_put(out, (const char *)GUEST_RM, GUEST_RM_LEN);
    else if (strcmp(path, "/bin/touch") == 0)
        buf_put(out, (const char *)GUEST_TOUCH, GUEST_TOUCH_LEN);
    else if (strcmp(path, "/bin/grep") == 0)
        buf_put(out, (const char *)GUEST_GREP, GUEST_GREP_LEN);
    else if (strcmp(path, "/bin/sed") == 0)
        buf_put(out, (const char *)GUEST_SED, GUEST_SED_LEN);
    else if (strcmp(path, "/bin/ed") == 0)
        buf_put(out, (const char *)GUEST_ED, GUEST_ED_LEN);
    else if (strcmp(path, "/bin/rb") == 0)
        buf_put(out, (const char *)GUEST_RB, GUEST_RB_LEN);
    else if (strcmp(path, "/bin/py") == 0)
        buf_put(out, (const char *)GUEST_PY, GUEST_PY_LEN);
    else if (strcmp(path, "/root/examples/README") == 0)
        buf_puts(out,
        "/root/examples -- scripts that work.\n"
        "\n"
        "  sh /root/examples/watch.sh          what is going on\n"
        "  run /root/examples/gunner.py        fires the gun for you, forever\n"
        "  run /root/examples/firewatch.py     sends somebody to the worst fire\n"
        "\n"
        "This is the ship's computer. It is a real machine: ls, cat, grep, pipes, for\n"
        "loops, and files you can edit. Two things on it are not ordinary:\n"
        "\n"
        "  rb    the ship, from the command line. Every command the buttons send.\n"
        "        `rb help` lists all of them. `rb ship` prints the lot.\n"
        "\n"
        "  py    a Python subset -- if/elif/else, while, for/in, def, integers,\n"
        "        strings, lists and dicts. `py` on its own lists what it knows.\n"
        "\n"
        "THE DIFFERENCE BETWEEN sh AND run:\n"
        "\n"
        "  sh py thing.py     runs it now, to completion, while the fight waits\n"
        "  run thing.py       starts it running IN the fight, alongside everything\n"
        "\n"
        "The second one is the one that matters, and it costs power: scripts run on\n"
        "this computer, and this computer draws from the same reactor as the shields.\n"
        "No bars in the computer and nothing you wrote runs at all.\n"
        "\n"
        "The shortest useful thing you can type:\n"
        "\n"
        "  rb ship\n"
        "\n"
        "The shortest useful thing you can leave running:\n"
        "\n"
        "  while True:\n"
        "      s = json(ship())\n"
        "      if s[\"weapon\"] == \"100\":\n"
        "          do(\"fire\")\n"
        "\n");
    else if (strcmp(path, "/root/examples/watch.sh") == 0)
        buf_puts(out,
        "# /root/examples/watch.sh\n"
        "#\n"
        "# The smallest useful thing, and a good first script: what is going on.\n"
        "#\n"
        "#   sh /root/examples/watch.sh\n"
        "#\n"
        "# Everything here is /bin/rb, which sends the same commands the buttons send.\n"
        "# If you can click it you can type it; if you can type it you can put it in a\n"
        "# file like this one.\n"
        "\n"
        "echo \"--- the ship ---\"\n"
        "rb ship\n"
        "echo\n"
        "echo \"--- who is where ---\"\n"
        "rb crew\n"
        "echo\n"
        "echo \"--- what just happened ---\"\n"
        "rb log\n"
        "\n");
    else if (strcmp(path, "/root/examples/selftest.py") == 0)
        buf_puts(out,
        "# /root/examples/selftest.py -- does this language actually work?\n"
        "#\n"
        "# Run it: py /root/examples/selftest.py\n"
        "#\n"
        "# It is here for two reasons. The first is that a scripting language you\n"
        "# cannot trust is worse than none: a player debugging their own logic against\n"
        "# an interpreter that is quietly wrong will conclude they cannot program, and\n"
        "# that is the exact opposite of what this game is for.\n"
        "#\n"
        "# The second is that it is the clearest documentation of what the language\n"
        "# HAS. Everything below is a thing you can use.\n"
        "\n"
        "# SELF-COUNTING, because the first version of this file had a hardcoded total\n"
        "# and got it wrong -- it reported \"59 of 62 checks passed\" while every single\n"
        "# check was passing. A test that can be wrong about its own arithmetic is a\n"
        "# test that will one day be wrong about yours.\n"
        "results = []\n"
        "\n"
        "def check(name, got, want):\n"
        "    if got == want:\n"
        "        append(results, 1)\n"
        "        return 1\n"
        "    print(\"FAIL\", name, \"got\", got, \"want\", want)\n"
        "    append(results, 0)\n"
        "    return 0\n"
        "\n"
        "# --- arithmetic -----------------------------------------------------------\n"
        "check(\"add\", 2 + 3, 5)\n"
        "check(\"sub\", 10 - 4, 6)\n"
        "check(\"mul\", 6 * 7, 42)\n"
        "check(\"div\", 9 / 2, 4)\n"
        "check(\"floordiv\", 9 // 2, 4)\n"
        "check(\"mod\", 9 % 4, 1)\n"
        "check(\"pow\", 2 ** 10, 1024)\n"
        "check(\"neg\", 0 - 5, -5)\n"
        "check(\"precedence\", 2 + 3 * 4, 14)\n"
        "check(\"parens\", (2 + 3) * 4, 20)\n"
        "\n"
        "# --- comparison and logic -------------------------------------------------\n"
        "check(\"lt\", 1 < 2, True)\n"
        "check(\"gt\", 1 > 2, False)\n"
        "check(\"le\", 2 <= 2, True)\n"
        "check(\"ge\", 2 >= 3, False)\n"
        "check(\"eq\", 3 == 3, True)\n"
        "check(\"ne\", 3 != 3, False)\n"
        "check(\"and\", True and False, False)\n"
        "check(\"or\", True or False, True)\n"
        "check(\"not\", not False, True)\n"
        "\n"
        "# --- variables and assignment ---------------------------------------------\n"
        "x = 1\n"
        "x = x + 1\n"
        "check(\"assign\", x, 2)\n"
        "x += 3\n"
        "check(\"pluseq\", x, 5)\n"
        "x -= 1\n"
        "check(\"minuseq\", x, 4)\n"
        "x *= 2\n"
        "check(\"stareq\", x, 8)\n"
        "\n"
        "# --- strings --------------------------------------------------------------\n"
        "s = \"hello\"\n"
        "check(\"strlen\", len(s), 5)\n"
        "check(\"concat\", \"ab\" + \"cd\", \"abcd\")\n"
        "check(\"streq\", \"abc\" == \"abc\", True)\n"
        "check(\"sub\", sub(\"abcdef\", 1, 3), \"bc\")\n"
        "check(\"sub-tail\", sub(\"abcdef\", 3), \"def\")\n"
        "check(\"lower\", lower(\"ABC\"), \"abc\")\n"
        "check(\"upper\", upper(\"abc\"), \"ABC\")\n"
        "check(\"find-hit\", find(\"hello world\", \"world\"), 6)\n"
        "check(\"find-miss\", find(\"hello\", \"zzz\"), -1)\n"
        "check(\"str-of-int\", str(42), \"42\")\n"
        "check(\"int-of-str\", int(\"42\"), 42)\n"
        "check(\"int-negative\", int(\"-7\"), -7)\n"
        "\n"
        "# --- lists ----------------------------------------------------------------\n"
        "l = [1, 2, 3]\n"
        "check(\"list-len\", len(l), 3)\n"
        "check(\"list-index\", l[0], 1)\n"
        "check(\"list-last\", l[2], 3)\n"
        "l[1] = 20\n"
        "check(\"list-set\", l[1], 20)\n"
        "\n"
        "total = 0\n"
        "for v in l:\n"
        "    total = total + v\n"
        "check(\"list-for\", total, 24)\n"
        "\n"
        "# --- dicts ----------------------------------------------------------------\n"
        "d = {\"a\": 1, \"b\": 2}\n"
        "check(\"dict-len\", len(d), 2)\n"
        "check(\"dict-get\", d[\"a\"], 1)\n"
        "d[\"c\"] = 3\n"
        "check(\"dict-set\", d[\"c\"], 3)\n"
        "\n"
        "# --- control flow ---------------------------------------------------------\n"
        "n = 0\n"
        "if 1 < 2:\n"
        "    n = 1\n"
        "elif 1 < 3:\n"
        "    n = 2\n"
        "else:\n"
        "    n = 3\n"
        "check(\"if\", n, 1)\n"
        "\n"
        "n = 0\n"
        "if 2 < 1:\n"
        "    n = 1\n"
        "elif 1 < 3:\n"
        "    n = 2\n"
        "else:\n"
        "    n = 3\n"
        "check(\"elif\", n, 2)\n"
        "\n"
        "n = 0\n"
        "if 2 < 1:\n"
        "    n = 1\n"
        "else:\n"
        "    n = 3\n"
        "check(\"else\", n, 3)\n"
        "\n"
        "i = 0\n"
        "acc = 0\n"
        "while i < 5:\n"
        "    acc = acc + i\n"
        "    i = i + 1\n"
        "check(\"while\", acc, 10)\n"
        "\n"
        "acc = 0\n"
        "for i in [1, 2, 3, 4, 5]:\n"
        "    if i == 3:\n"
        "        continue\n"
        "    if i == 5:\n"
        "        break\n"
        "    acc = acc + i\n"
        "check(\"break-continue\", acc, 7)\n"
        "\n"
        "# --- functions ------------------------------------------------------------\n"
        "def double(v):\n"
        "    return v * 2\n"
        "\n"
        "def addup(a, b):\n"
        "    return a + b\n"
        "\n"
        "def fib(n):\n"
        "    if n < 2:\n"
        "        return n\n"
        "    return fib(n - 1) + fib(n - 2)\n"
        "\n"
        "check(\"call\", double(21), 42)\n"
        "check(\"two-args\", addup(2, 3), 5)\n"
        "check(\"recursion\", fib(10), 55)\n"
        "\n"
        "def early(v):\n"
        "    if v:\n"
        "        return \"yes\"\n"
        "    return \"no\"\n"
        "\n"
        "check(\"early-return\", early(True), \"yes\")\n"
        "check(\"no-return-path\", early(False), \"no\")\n"
        "\n"
        "# --- the ship, which is why the language is here --------------------------\n"
        "s = json(ship())\n"
        "check(\"ship-answers\", has(s, \"hull\"), True)\n"
        "check(\"ship-has-rooms\", find(ship(), \"shields\") > 0, True)\n"
        "check(\"do-works\", find(do(\"power shields 2\"), \"+OK\"), 0)\n"
        "check(\"do-refuses\", find(do(\"power shields 99\"), \"-ERR\"), 0)\n"
        "\n"
        "parts = split(\"a,b,c\", \",\")\n"
        "check(\"split-count\", len(parts), 3)\n"
        "check(\"split-mid\", parts[1], \"b\")\n"
        "\n"
        "# --- lists you build, dicts you walk --------------------------------------\n"
        "built = []\n"
        "for i in [1, 2, 3]:\n"
        "    append(built, i * i)\n"
        "check(\"append\", len(built), 3)\n"
        "check(\"append-value\", built[2], 9)\n"
        "check(\"join\", join([\"a\", \"b\", \"c\"], \"-\"), \"a-b-c\")\n"
        "\n"
        "d2 = {\"x\": 1}\n"
        "d2[\"y\"] = 2\n"
        "ks = keys(d2)\n"
        "check(\"keys-count\", len(ks), 2)\n"
        "check(\"keys-first\", ks[0], \"x\")\n"
        "check(\"has-yes\", has(d2, \"x\"), True)\n"
        "check(\"has-no\", has(d2, \"zzz\"), False)\n"
        "\n"
        "check(\"strip\", strip(\"  padded  \"), \"padded\")\n"
        "check(\"replace\", replace(\"a-b-c\", \"-\", \"+\"), \"a+b+c\")\n"
        "check(\"replace-delete\", replace(\"hello\", \"l\", \"\"), \"heo\")\n"
        "\n"
        "# --- files, because a script that cannot keep a note is told everything twice\n"
        "write(\"/tmp/selftest.txt\", \"kept\")\n"
        "check(\"read-back\", read(\"/tmp/selftest.txt\"), \"kept\")\n"
        "check(\"read-missing\", read(\"/tmp/does-not-exist\"), nil)\n"
        "\n"
        "# --- the report -----------------------------------------------------------\n"
        "passed = 0\n"
        "for v in results:\n"
        "    passed = passed + v\n"
        "print(\"selftest:\", passed, \"of\", len(results), \"checks passed\")\n"
        "if passed == len(results):\n"
        "    print(\"selftest: OK\")\n"
        "else:\n"
        "    print(\"selftest: FAILED\")\n"
        "\n");
    else if (strcmp(path, "/root/examples/selftest.sh") == 0)
        buf_puts(out,
        "# /root/examples/selftest.sh -- does the shell actually work?\n"
        "#\n"
        "#   sh /root/examples/selftest.sh\n"
        "#\n"
        "# The same argument as selftest.py: a shell you cannot trust teaches a player\n"
        "# that they cannot script, which is the opposite of the point. Everything\n"
        "# below is a thing you can use in your own scripts.\n"
        "\n"
        "echo \"--- echo and quoting ---\"\n"
        "echo hello world\n"
        "echo \"quoted string with spaces\"\n"
        "echo 'single quotes too'\n"
        "\n"
        "echo \"--- variables ---\"\n"
        "NAME=kestrel\n"
        "echo $NAME\n"
        "echo \"in a string: $NAME\"\n"
        "\n"
        "echo \"--- command substitution ---\"\n"
        "HOST=$(cat /etc/hostname)\n"
        "echo \"host is $HOST\"\n"
        "\n"
        "echo \"--- pipelines ---\"\n"
        "echo one two three | wc\n"
        "echo abc | rev\n"
        "echo abc | rev | rev\n"
        "\n"
        "echo \"--- redirection ---\"\n"
        "echo first > /tmp/sh_test\n"
        "echo second >> /tmp/sh_test\n"
        "cat /tmp/sh_test\n"
        "\n"
        "echo \"--- for loops ---\"\n"
        "for i in a b c; do echo \"loop $i\"; done\n"
        "\n"
        "echo \"--- and, or, exit status ---\"\n"
        "true && echo \"and ran\"\n"
        "false || echo \"or ran\"\n"
        "true\n"
        "echo \"status $?\"\n"
        "\n"
        "echo \"--- globbing ---\"\n"
        "ls /bin/rb\n"
        "\n"
        "echo \"--- calling the ship ---\"\n"
        "rb ship | head\n"
        "\n"
        "echo \"--- a loop over the rooms, which is the first real script anybody writes\"\n"
        "for r in $(rb rooms | grep fire); do echo \"saw a room\"; done\n"
        "\n"
        "echo \"selftest.sh: done\"\n"
        "\n");
    else if (strcmp(path, "/root/examples/gunner.py") == 0)
        buf_puts(out,
        "# /root/examples/gunner.py\n"
        "#\n"
        "# The first script worth leaving running, and it is four lines of work.\n"
        "#\n"
        "#   run /root/examples/gunner.py\n"
        "#\n"
        "# It watches the gun and fires it the moment it is charged, for the rest of\n"
        "# the fight, while you deal with the fire in the engine room.\n"
        "#\n"
        "# WHAT IT COSTS. It runs on the ship's computer, and the computer runs on\n"
        "# reactor bars -- the same bars the shields want. With no power in the\n"
        "# computer this does nothing at all. That is not a bug and it is the most\n"
        "# important number in the game: automation is a trade, not a cheat.\n"
        "#\n"
        "# Give the computer a bar and see.\n"
        "\n"
        "while True:\n"
        "    s = json(ship())\n"
        "    if s[\"over\"] == \"true\":\n"
        "        exit(0)\n"
        "    if s[\"weapon\"] == \"100\":\n"
        "        # Their shields first, so the next one gets through to the hull.\n"
        "        if int(s[\"shields\"]) >= 0:\n"
        "            do(\"fire\")\n"
        "\n");
    else if (strcmp(path, "/root/examples/firewatch.py") == 0)
        buf_puts(out,
        "# /root/examples/firewatch.py\n"
        "#\n"
        "# The second script anybody writes, and the one that wins fights.\n"
        "#\n"
        "#   run /root/examples/firewatch.py\n"
        "#\n"
        "# Somebody idle goes to the worst fire. That is the whole thing -- and it is\n"
        "# the job you were doing by hand, badly, while also trying to aim.\n"
        "#\n"
        "# It is deliberately not clever. Read it, then make it better: it does not\n"
        "# care who is nearest, it does not stop somebody bleeding to death in a room\n"
        "# with no air, and it will happily send your only engineer into a fire.\n"
        "\n"
        "def worst_fire(rooms):\n"
        "    at = -1\n"
        "    most = 5\n"
        "    for r in rooms:\n"
        "        f = int(r[\"fire\"])\n"
        "        if f > most:\n"
        "            most = f\n"
        "            at = int(r[\"n\"])\n"
        "    return at\n"
        "\n"
        "while True:\n"
        "    s = json(ship())\n"
        "    if s[\"over\"] == \"true\":\n"
        "        exit(0)\n"
        "\n"
        "    rooms = []\n"
        "    for l in lines(do(\"rooms\")):\n"
        "        if find(l, \"{\") == 0:\n"
        "            append(rooms, json(l))\n"
        "\n"
        "    burning = worst_fire(rooms)\n"
        "    if burning >= 0:\n"
        "        for l in lines(do(\"crew\")):\n"
        "            if find(l, \"{\") == 0:\n"
        "                c = json(l)\n"
        "                if c[\"alive\"] == \"true\":\n"
        "                    do(\"send \" + c[\"name\"] + \" \" + str(burning))\n"
        "\n");
    else if (strcmp(path, "/bin/echo") == 0)
        buf_put(out, (const char *)GUEST_ECHO, GUEST_ECHO_LEN);
    else if (strcmp(path, "/bin/wc") == 0)
        buf_put(out, (const char *)GUEST_WC, GUEST_WC_LEN);
    else if (strcmp(path, "/bin/head") == 0)
        buf_put(out, (const char *)GUEST_HEAD, GUEST_HEAD_LEN);
    else if (strcmp(path, "/bin/tail") == 0)
        buf_put(out, (const char *)GUEST_TAIL, GUEST_TAIL_LEN);
    else if (strcmp(path, "/bin/du") == 0)
        buf_put(out, (const char *)GUEST_DU, GUEST_DU_LEN);
    else if (strcmp(path, "/bin/mkdir") == 0)
        buf_put(out, (const char *)GUEST_MKDIR, GUEST_MKDIR_LEN);
    else if (strcmp(path, "/bin/rev") == 0)
        buf_put(out, (const char *)GUEST_REV, GUEST_REV_LEN);
    else if (strcmp(path, "/bin/seq") == 0)
        buf_put(out, (const char *)GUEST_SEQ, GUEST_SEQ_LEN);
    else if (strcmp(path, "/usr/bin/rot13") == 0)
        buf_put(out, (const char *)GUEST_ROT13, GUEST_ROT13_LEN);
    else if (strcmp(path, "/bin/uname") == 0)
        buf_put(out, (const char *)GUEST_UNAME, GUEST_UNAME_LEN);
    else if (strcmp(path, "/bin/whoami") == 0)
        buf_put(out, (const char *)GUEST_WHOAMI, GUEST_WHOAMI_LEN);
    else if (strcmp(path, "/bin/df") == 0)
        buf_put(out, (const char *)GUEST_DF, GUEST_DF_LEN);
    else if (strcmp(path, "/usr/sbin/syslogd") == 0)
        buf_put(out, (const char *)GUEST_SYSLOGD, GUEST_SYSLOGD_LEN);
    else if (strcmp(path, "/usr/sbin/netd") == 0)
        buf_put(out, (const char *)GUEST_NETD, GUEST_NETD_LEN);
    else if (strcmp(path, "/usr/sbin/udevd") == 0)
        buf_put(out, (const char *)GUEST_UDEVD, GUEST_UDEVD_LEN);
    else if (strcmp(path, "/usr/sbin/crond") == 0)
        buf_put(out, (const char *)GUEST_CROND, GUEST_CROND_LEN);
    else if (strcmp(path, "/usr/sbin/ntpd") == 0)
        buf_put(out, (const char *)GUEST_NTPD, GUEST_NTPD_LEN);
    else if (strcmp(path, "/usr/sbin/httpd") == 0)
        buf_put(out, (const char *)GUEST_HTTPD, GUEST_HTTPD_LEN);
    else if (strcmp(path, "/usr/sbin/nft") == 0)
        buf_put(out, (const char *)GUEST_NFT, GUEST_NFT_LEN);
    else if (strcmp(path, "/usr/sbin/auditd") == 0)
        buf_put(out, (const char *)GUEST_AUDITD, GUEST_AUDITD_LEN);
    else if (strcmp(path, "/usr/sbin/sshd") == 0)
        buf_put(out, (const char *)GUEST_SSHD, GUEST_SSHD_LEN);
    else if (strcmp(path, "/usr/sbin/postfix") == 0)
        buf_put(out, (const char *)GUEST_POSTFIX, GUEST_POSTFIX_LEN);
    else if (strcmp(path, "/usr/bin/pkg") == 0)
        buf_put(out, (const char *)GUEST_PKG, GUEST_PKG_LEN);
    else if (strcmp(path, "/usr/bin/links") == 0)
        buf_put(out, (const char *)GUEST_LINKS, GUEST_LINKS_LEN);
    else if (strcmp(path, "/usr/bin/man") == 0)
        buf_put(out, (const char *)GUEST_MAN, GUEST_MAN_LEN);
    else if (strcmp(path, "/usr/bin/ldd") == 0)
        buf_put(out, (const char *)GUEST_LDD, GUEST_LDD_LEN);
    else if (strcmp(path, "/bin/dmesg") == 0)
        buf_put(out, (const char *)GUEST_DMESG, GUEST_DMESG_LEN);
    else if (strcmp(path, "/usr/bin/rcon") == 0)
        buf_put(out, (const char *)GUEST_RCON, GUEST_RCON_LEN);
    else if (strcmp(path, "/usr/bin/find") == 0)
        buf_put(out, (const char *)GUEST_FIND, GUEST_FIND_LEN);
    else if (strcmp(path, "/bin/netstat") == 0)
        buf_put(out, (const char *)GUEST_NETSTAT, GUEST_NETSTAT_LEN);
    else if (strcmp(path, "/bin/ping") == 0)
        buf_put(out, (const char *)GUEST_PING, GUEST_PING_LEN);
    else if (strcmp(path, "/bin/ip") == 0)
        buf_put(out, (const char *)GUEST_IP, GUEST_IP_LEN);
    else if (strcmp(path, "/bin/arp") == 0)
        buf_put(out, (const char *)GUEST_ARP, GUEST_ARP_LEN);
    else if (strcmp(path, "/bin/traceroute") == 0)
        buf_put(out, (const char *)GUEST_TRACEROUTE, GUEST_TRACEROUTE_LEN);
    else if (strcmp(path, "/usr/sbin/tcpdump") == 0)
        buf_put(out, (const char *)GUEST_TCPDUMP, GUEST_TCPDUMP_LEN);
    else if (strcmp(path, "/bin/ss") == 0)
        buf_put(out, (const char *)GUEST_SS, GUEST_SS_LEN);
    else if (strcmp(path, "/bin/voice") == 0)
        buf_put(out, (const char *)GUEST_VOICE, GUEST_VOICE_LEN);
    else if (strcmp(path, "/usr/bin/nomde") == 0)
        buf_put(out, (const char *)GUEST_NOMDE, GUEST_NOMDE_LEN);
    else if (strcmp(path, "/usr/bin/open") == 0)
        buf_put(out, (const char *)GUEST_OPEN, GUEST_OPEN_LEN);
    else if (strcmp(path, "/usr/bin/fortune") == 0)
        buf_put(out, (const char *)GUEST_FORTUNE, GUEST_FORTUNE_LEN);
    else if (strcmp(path, "/usr/bin/cowsay") == 0)
        buf_put(out, (const char *)GUEST_COWSAY, GUEST_COWSAY_LEN);
    else if (strcmp(path, "/usr/bin/sl") == 0)
        buf_put(out, (const char *)GUEST_SL, GUEST_SL_LEN);
    else if (strcmp(path, "/sbin/telinit") == 0)
        buf_put(out, (const char *)GUEST_INIT, GUEST_INIT_LEN);
    else if (strcmp(path, "/sbin/reboot") == 0 ||
             strcmp(path, "/sbin/halt") == 0 ||
             strcmp(path, "/sbin/poweroff") == 0)
        buf_put(out, (const char *)GUEST_REBOOT, GUEST_REBOOT_LEN);
    else if (strcmp(path, "/usr/sbin/zbl-install") == 0)
        buf_put(out, (const char *)GUEST_ZBL_INSTALL, GUEST_ZBL_INSTALL_LEN);
    else if (strcmp(path, "/usr/sbin/zbl-mkconfig") == 0)
        buf_put(out, (const char *)GUEST_ZBL_MKCONFIG, GUEST_ZBL_MKCONFIG_LEN);
    else if (strcmp(path, "/usr/bin/mkinitrd") == 0)
        buf_put(out, (const char *)GUEST_MKINITRD, GUEST_MKINITRD_LEN);
    else if (strcmp(path, "/sbin/svcinit") == 0)
        buf_put(out, (const char *)GUEST_SVCINIT, GUEST_SVCINIT_LEN);
    else if (strcmp(path, "/sbin/login") == 0)
        buf_put(out, (const char *)GUEST_LOGIN, GUEST_LOGIN_LEN);
    else if (strcmp(path, "/sbin/getty") == 0)
        buf_put(out, (const char *)GUEST_GETTY, GUEST_GETTY_LEN);
    else if (strcmp(path, "/sbin/fsck") == 0)
        buf_put(out, (const char *)GUEST_FSCK, GUEST_FSCK_LEN);
    else if (strcmp(path, "/sbin/blkid") == 0)
        buf_put(out, (const char *)GUEST_BLKID, GUEST_BLKID_LEN);
    else if (strcmp(path, "/bin/kill") == 0)
        buf_put(out, (const char *)GUEST_KILL, GUEST_KILL_LEN);
    else if (strcmp(path, "/usr/bin/svc") == 0)
        buf_put(out, (const char *)GUEST_SVC, GUEST_SVC_LEN);
    else if (strcmp(path, "/sbin/mountall") == 0)
        buf_put(out, (const char *)GUEST_MOUNTALL, GUEST_MOUNTALL_LEN);
    else if (strcmp(path, "/etc/rc.boot") == 0)       buf_puts(out, SRC_RCBOOT);
    else if (strcmp(path, "/etc/rc.d/rc.3") == 0)     buf_puts(out, SRC_RC3);
    else if (strcmp(path, "/etc/rc.d/rc.0") == 0)     buf_puts(out, SRC_RC0);
    else if (strcmp(path, "/boot/zbl/zbl.cfg") == 0) {
        buf_puts(out, "default 0\ntimeout 5\n\n");
        buf_puts(out, "entry \"NomnixOS 11.4\"\n");
        buf_puts(out, "  kernel /boot/vmnomuz\n");
        buf_puts(out, "  initrd /boot/initrd\n");
        buf_puts(out, "  root UUID=");
        buf_puts(out, m->root_uuid);
        buf_puts(out, "\n");
    } else if (strcmp(path, "/etc/fstab") == 0) {
        buf_puts(out, "# device                        mount  type  options\n");
        buf_puts(out, "UUID=");
        buf_puts(out, m->root_uuid);
        buf_puts(out, "  /      ext4  defaults\n");
        buf_puts(out, "none                            /proc  proc  defaults\n");
        buf_puts(out, "none                            /tmp   tmpfs defaults\n");
        buf_puts(out, "/dev/sr0                        /media iso9660 noauto\n");
    } else if (strcmp(path, "/etc/hostname") == 0) {
        buf_puts(out, "node-");
        buf_puts(out, m->id);
        buf_puts(out, "\n");
    }
}

static void pristine(const Machine *m, const PkgFile *f, Buf *out);

static void install_file(Machine *m, const PkgFile *f)
{
    if (f->isdir) {
        VNode *n = vfs_mkdir(&m->disk, f->path);
        if (n) n->mode = f->mode;
        return;
    }
    if (f->link) { vfs_symlink(&m->disk, f->link, f->path); return; }
    if (f->content) {
        VNode *n = vfs_mkfile(&m->disk, f->path, f->content);
        if (n) n->mode = f->mode;
        return;
    }
    /* Generated content may be a BINARY (the guest programs are ELF images
     * with embedded NULs), so it is written by length, never as a C string.
     * Going through vfs_mkfile's char* would truncate every binary at its
     * first zero byte. */
    Buf b = {0};
    image_generated(m, f->path, &b);
    VNode *n = vfs_mkfile(&m->disk, f->path, "");
    if (n) {
        buf_clear(&n->data);
        buf_put(&n->data, b.p, b.len);
        n->mode = f->mode;
    }
    buf_free(&b);
}

/* FNV-1a, the same hash /usr/bin/pkg computes on the guest side. If these two
 * ever disagree, verify reports a clean machine as broken -- so they are the
 * same three lines, deliberately trivial. */
static uint64_t fnv1a(const char *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; }
    return h;
}

/* The package database, written onto the machine's own disk at
 * /var/lib/pkg/<name>/{version,files}. It is real data that a real program
 * reads -- which also means it can be damaged, and `pkg verify` says so
 * rather than reporting a clean system. */
static void install_pkgdb(Machine *m)
{
    for (int i = 0; i < m->npkg; i++) {
        const Package *p = m->pkg[i];
        char dir[NOM_PATH_MAX], fp[NOM_PATH_MAX];
        snprintf(dir, sizeof dir, "/var/lib/pkg/%s", p->name);
        vfs_mkdir(&m->disk, dir);

        snprintf(fp, sizeof fp, "%s/version", dir);
        char ver[128];
        snprintf(ver, sizeof ver, "%s  %s\n", p->version, p->desc);
        VNode *vn = vfs_mkfile(&m->disk, fp, ver);
        if (vn) vn->mode = 0644;

        Buf man = {0};
        for (int j = 0; j < p->nfiles; j++) {
            const PkgFile *f = &p->file[j];
            if (f->link) {
                /* A symlink has no content, but it very much has a value, and
                 * a deleted or repointed one is one of the commonest ways a
                 * machine stops booting. Recording the hash of its TARGET is
                 * what lets verify see that. */
                buf_printf(&man, "link %016llx %s\n",
                           (unsigned long long)fnv1a(f->link, strlen(f->link)),
                           f->path);
                continue;
            }
            if (f->isdir) {
                /* Three fields like every other line; the mode goes where a
                 * file's hash would, because a directory has no contents to
                 * hash and its mode is the thing worth checking. */
                buf_printf(&man, "dir %04o %s\n", f->mode, f->path);
                continue;
            }
            Buf c = {0};
            pristine(m, f, &c);
            buf_printf(&man, "%04o %016llx %s\n", f->mode,
                       (unsigned long long)fnv1a(c.p, c.len), f->path);
            buf_free(&c);
        }
        snprintf(fp, sizeof fp, "%s/files", dir);
        VNode *fn = vfs_mkfile(&m->disk, fp, "");
        if (fn) {
            buf_clear(&fn->data);
            buf_put(&fn->data, man.p, man.len);
            fn->mode = 0644;
        }
        buf_free(&man);
    }
}

void machine_install(Machine *m, uint64_t seed)
{
    memset(m, 0, sizeof *m);
    vfs_init(&m->disk);
    snprintf(m->id, sizeof m->id, "%llu", (unsigned long long)(seed % 10000));
    snprintf(m->root_uuid, sizeof m->root_uuid, "%s", ROOT_UUID);
    m->bootsector = true;

    static const char *DIRS[] = {
        "/bin", "/boot", "/boot/zbl", "/dev", "/etc", "/etc/nomde",
        "/usr/share/applications", "/run/nomde",
        "/etc/net", "/etc/rc.d", "/etc/services.d", "/etc/ssh", "/etc/udev",
        "/etc/udev/rules.d", "/home", "/home/nomowner", "/home/nomowner/bin",
        /* A half-finished project gets a directory, the way a half-finished
         * project always does: a README, the thing that is not a program,
         * and the list of what it would have needed. */
        "/home/nomowner/bin/logsweep",
        /* A HOME DIRECTORY LOOKS LIKE SOMEBODY LIVES IN IT. Everything was
         * dumped straight in /home/nomowner, which is not what anyone's home
         * looks like -- there is always a Desktop, always a Documents, always
         * a Downloads full of things they meant to sort out. */
        "/home/nomowner/Desktop", "/home/nomowner/Documents",
        "/home/nomowner/Downloads", "/home/nomowner/Pictures",
        "/root/Desktop", "/root/Documents", "/root/Downloads",
        "/lib", "/lib/modules",
        "/lib/modules/6.4.11", "/proc", "/root", "/sbin", "/sys", "/tmp",
        "/mnt", "/media", "/usr", "/usr/bin", "/usr/lib", "/usr/lib/sysinit",
        "/usr/sbin", "/usr/share", "/usr/share/man", "/usr/share/zoneinfo",
        /* /usr/share/doc/<package>, exactly as every real distribution lays
         * it out and named exactly as `pkg list` names the package. The docs
         * are shipped BY the package they document, so `pkg owns` answers,
         * `pkg verify` hashes them and an edited doc reads as CHANGED --
         * which is what stops them being a second source of truth sitting
         * beside the machine and disagreeing with it. */
        "/usr/share/doc", "/usr/share/doc/libc", "/usr/share/doc/zlib",
        "/usr/share/doc/httpd", "/usr/share/doc/kernel-default",
        "/usr/share/doc/syslog", "/usr/share/doc/cron",
        "/usr/share/doc/pkg-config-data", "/usr/share/doc/sysinit",
        "/usr/share/doc/shadow", "/usr/share/doc/nomsh",
        "/usr/share/doc/nomfun", "/usr/share/doc/zbl",
        /* A doc file whose directory is not on this list HALF EXISTS: `ls`
         * lists it and `stat` reports its mode and its size, and every
         * reader -- cat, head, man, pkg diff -- answers "cannot read". That
         * is a worse failure than a missing file, because the two tools you
         * reach for to find out whether it is there both say it is. */
        "/usr/share/doc/netcfg",
        "/usr/share/terminfo", "/var", "/var/log", "/var/lib", "/var/lib/ntp",
        "/var/lib/pkg", "/var/cache", "/var/spool", "/var/spool/cron",
        "/etc/audit", "/etc/default", "/etc/httpd", "/etc/logrotate.d",
        "/etc/postfix", "/srv", "/srv/www", "/etc/pkg", "/etc/pkg/repos.d",
        "/run",
        /* RUNBOOK: declared, not left to vfs_mkfile to invent. A directory
         * created implicitly as a side effect of writing a file in it gets no
         * mode, so every file under /root/examples existed, had a size, and
         * could not be opened -- `ls` showed them and `cat` said "cannot
         * read", which is a confusing pair of answers to get about the same
         * file. */
        "/root/examples", "/root/scripts", "/root/Desktop",
        "/home/pvane", "/home/pvane/Documents", "/home/pvane/bin",
        NULL
    };
    for (int i = 0; DIRS[i]; i++) vfs_mkdir(&m->disk, DIRS[i]);

    for (int i = 0; i < IMAGE_N && i < PKG_MAX; i++) {
        /* A package whose nfiles does not match its initialiser list either
         * silently drops files (invisible to pkg verify, unrepairable by
         * reinstall) or reads past the end of the array. Both have happened.
         * Neither is worth debugging twice. */
        for (int j = 0; j < IMAGE[i]->nfiles; j++) {
            if (IMAGE[i]->file[j].path) continue;
            fprintf(stderr, "image: package %s declares %d files but entry %d "
                            "is empty -- fix its count\n",
                    IMAGE[i]->name, IMAGE[i]->nfiles, j);
            abort();
        }
        /* AND THE OTHER DIRECTION, which this check did not cover and which
         * cost an hour: a list LONGER than its count drops the tail
         * silently. Adding a man page to man-db without touching the 20
         * below it deleted rev(1) from the machine, and the new page went in
         * unreadable -- `man` listed it and `man fsck` said no such entry.
         * Nothing anywhere said a file had gone. */
        if (IMAGE[i]->nfiles < PKGFILE_MAX && IMAGE[i]->file[IMAGE[i]->nfiles].path) {
            fprintf(stderr, "image: package %s declares %d files but entry %d "
                            "(%s) is still there -- the tail of the list is "
                            "being dropped; fix its count\n",
                    IMAGE[i]->name, IMAGE[i]->nfiles, IMAGE[i]->nfiles,
                    IMAGE[i]->file[IMAGE[i]->nfiles].path);
            abort();
        }
        m->pkg[m->npkg++] = IMAGE[i];
        for (int j = 0; j < IMAGE[i]->nfiles; j++)
            install_file(m, &IMAGE[i]->file[j]);
    }
    /* Sized from what the installation actually takes, with room for a
     * working machine and not much more -- which is what a real disk feels
     * like and is what makes filling it possible. */
    m->fs_capacity = 0;
    install_pkgdb(m);
    install_history(m);
    install_local_edits(m, seed);
    m->fs_capacity = machine_disk_used(m) + 512u * 1024u;
    /* Headroom in inodes as well as bytes, so a healthy machine can create
     * files freely and a fault has to work to exhaust them. */
    m->fs_inodes_max = machine_inodes_used(m) + 400u;
    install_rescue(m);
    m->next_pid = 1;
}

/* THINGS NO PACKAGE OWNS, because in life no package owns them: logs.
 *
 * A machine that has been running since January has a log with January in it.
 * These are written straight onto the disk rather than shipped by a package,
 * which is not a shortcut -- it is the truth about what a log is, and `pkg
 * owns /var/log/messages` answering "nothing" is a fact worth being able to
 * discover. It also keeps them out of `pkg verify`: a log that matched a hash
 * would be a log nothing had written to.
 *
 * syslogd appends its own banner to /var/log/messages at every boot, so this
 * is history and the newest line is always the machine's own.
 */
static void install_history(Machine *m)
{
    /* DATED, BECAUSE IT IS OLD.
     *
     * These lines were undated and sat directly above the banner this boot's
     * syslogd appends, so the whole file read as live -- and a playtester on
     * a ticket where nomde was DEAD and had never started this boot read
     * `nomde: display server ready` off it and believed the display server
     * was fine. It was true when it was written, which is exactly what a log
     * is; nothing about the file said when that was.
     *
     * A timestamp on every line is what a real syslog looks like anyway, and
     * it makes the boundary unmistakable: everything with a date in front of
     * it happened on some previous day. What THIS boot did is in `dmesg`,
     * which is the real instrument and needs no defending. */
    VNode *n = vfs_mkfile(&m->disk, "/var/log/messages",
        "-- rotated by logrotate: every DATED line below is from an earlier\n"
        "-- day. What happened during THIS boot is in `dmesg`.\n"
        "Jul 28 04:02:11 syslogd: started, logging to /var/log/messages\n"
        "Jul 28 04:02:11 netd: eth0 configured\n"
        "Jul 28 22:14:03 sshd: refused connect from 10.0.2.88\n"
        "Jul 29 01:47:52 sshd: refused connect from 10.0.2.88\n"
        "Jul 30 03:09:40 sshd: refused connect from 10.0.2.88 (this went on for a week)\n"
        "Jul 30 06:25:00 ntpd: no reply from 10.0.2.3, will retry\n"
        "Jul 31 04:02:01 crond: (root) CMD (/usr/sbin/logrotate /etc/logrotate.conf)\n"
        "Jul 31 08:33:19 udevd: could not open /dev/input/event3: no such device\n"
        "Aug  1 09:00:02 httpd: document root /srv/www ok\n"
        "Aug  1 09:00:02 auditd: log opened\n"
        "Aug  1 09:00:03 nomde: display server ready\n");
    if (n) n->mode = 0644;

    /* The rotated one. logrotate.conf says weekly, rotate 8; this is what
     * came before, and it is the March outage as the machine saw it. */
    n = vfs_mkfile(&m->disk, "/var/log/messages.1",
        "syslogd: started, logging to /var/log/messages\n"
        "crond: (root) CMD (/usr/sbin/logrotate /etc/logrotate.conf)\n"
        "crond: (root) CMD (/home/nomowner/bin/cleanup)\n"
        "cleanup: removing stale kernel images\n"
        "cleanup: /boot/vmnomuz-6.4.11 removed\n"
        "cleanup: done, 1 file removed, 0 errors\n"
        "-- machine did not come back. 6 hours. See ~/TODO, item 3.\n"
        "syslogd: started, logging to /var/log/messages\n"
        "ntpd: no reply from 10.0.2.3, will retry\n"
        "ntpd: no reply from 10.0.2.3, will retry\n"
        "sshd: refused connect from 10.0.2.88\n"
        "udevd: could not open /dev/input/event3: no such device\n"
        "syslogd: /var/log/messages: cannot write -- is the disk full?\n"
        "-- it was. df said 100%, every hash matched, nothing was corrupt.\n"
        "-- growing since January. ~/Documents/postmortem-march.txt.\n"
        "syslogd: started, logging to /var/log/messages\n");
    if (n) n->mode = 0644;
}

/* Every real machine has been touched by a human. These are the edits that
 * admin made on purpose: a nameserver they changed, a service they turned
 * off, a host they added. They are legitimate, they are NOT the fault, and
 * `pkg verify` reports them as CHANGED because that is the truth.
 *
 * This is the single biggest thing standing between this game and a lookup
 * table. Before it, verify named exactly one file and that file was always
 * the answer. Now the player has to decide which difference MATTERS -- and
 * `pkg reinstall` on the wrong package silently destroys somebody's work.
 */
/* How many legitimate local edits exist, and a way to install exactly one of
 * them. `--health` walks all of them, because a decoy that breaks the machine
 * is a fairness bug of the worst kind: the player is told a deliberate-looking
 * edit is innocent by every signal the game gives, and it is the fault.
 *
 * One shipped. /etc/httpd/httpd.conf said `listen`/`root` where httpd wants
 * `Listen`/`DocumentRoot`, so that decoy silently killed the web server. It
 * survived a 20-machine health run because 17 decoys drawn 2-5 at a time do
 * not cover themselves in twenty tries. Now they are covered on purpose. */
int local_edit_count(void);

/* WHICH LINES OF A LOCAL EDIT ARE ACTUALLY LOCAL.
 *
 * See machine_collateral for why this exists. A decoy is a whole file, and
 * most of its lines are the ones the package ships -- the local decision is
 * the handful that are not. Those are the thing the collateral report is
 * about, and comparing whole files could never see them.
 *
 * A file no package owns has no shipped version, so every line of it is a
 * local decision, which is also the truth. */
static bool line_present(const Buf *hay, const char *line, size_t len)
{
    const char *p = hay->p, *end = hay->p ? hay->p + hay->len : NULL;
    while (p && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (ll == len && (len == 0 || memcmp(p, line, len) == 0)) return true;
        p = nl ? nl + 1 : end;
    }
    return false;
}

static void shipped_content(const Machine *m, const char *path, Buf *out)
{
    const Package *pk = pkg_owns(m, path);
    if (!pk) return;
    for (int j = 0; j < pk->nfiles; j++)
        if (strcmp(pk->file[j].path, path) == 0) {
            if (!pk->file[j].isdir && !pk->file[j].link)
                pristine(m, &pk->file[j], out);
            return;
        }
}

/* Record edit `path` with content `content` as local edit slot m->nlocal.
 * local_orig holds the LOCAL LINES, newline-terminated -- not the file. */
static void record_local(Machine *m, const char *path, const char *content)
{
    Buf ship = {0};
    shipped_content(m, path, &ship);
    Buf *keep = &m->local_orig[m->nlocal];
    buf_clear(keep);
    const char *p = content;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll && !line_present(&ship, p, ll)) {
            buf_put(keep, p, ll);
            buf_putc(keep, '\n');
        }
        p = nl ? nl + 1 : p + ll;
    }
    buf_free(&ship);
    snprintf(m->local[m->nlocal], NOM_PATH_MAX, "%s", path);
    m->nlocal++;
}

static void install_local_edits(Machine *m, uint64_t seed)
{
    Rng r;
    rng_seed(&r, seed ^ 0xc0ffee1234ULL);

    /* A wide pool, and SEVERAL WORDINGS EACH. A playtester reported that by
     * the fourth machine they filtered the decoys on sight without reading
     * them -- which is exactly right, because there were six files with one
     * fixed text apiece, so `/etc/ssh/sshd_config` always said "hardened
     * after the audit". A decoy you recognise is not a decoy, it is a
     * landmark. Rotating the wording is what makes you read the file. */
    struct { const char *path; const char *content; } EDITS[] = {
      { "/etc/resolv.conf",
        "# changed 12 March -- the .3 resolver was timing out at peak\n"
        "nameserver 10.0.2.9\n"
        "search nomnix.org\n" },
      { "/etc/resolv.conf",
        "nameserver 10.0.2.3\n"
        "# second one added after the outage in Feb, do not remove\n"
        "nameserver 10.0.2.9\n" },
      { "/etc/hosts",
        "127.0.0.1       localhost nominal.local\n"
        "10.0.2.20       wiki.nomnix.org wiki\n"
        "10.0.2.30       support.internal support\n"
        "10.0.2.44       bofh.nomnix.org bofh\n"
        "# added for the migration, remove when dock-2 is retired\n"
        "10.0.2.61       oldbilling.internal oldbilling\n" },
      { "/etc/hosts",
        "127.0.0.1       localhost\n"
        "10.0.2.20       wiki.nomnix.org wiki\n"
        "10.0.2.30       support.internal support\n"
        "10.0.2.44       bofh.nomnix.org bofh\n"
        "# pinning this until DNS is fixed -- J.\n"
        "10.0.2.31       licences.internal licences\n" },
      { "/etc/ssh/sshd_config",
        "# hardened after the audit, do not revert\n"
        "Port 2222\n"
        "PermitRootLogin no\n"
        "MaxAuthTries 3\n" },
      { "/etc/ssh/sshd_config",
        "Port 22\n"
        "# left root login on for the console cart -- ops asked, ticket 8841\n"
        "PermitRootLogin yes\n" },
      { "/etc/syslog.conf",
        "# quieten the udev chatter, it was filling the disk\n"
        "*.info /var/log/messages\n"
        "udev.* /dev/null\n" },
      { "/etc/syslog.conf",
        "*.info /var/log/messages\n"
        "# cron was noisy every minute, dropped it 4 Jan\n"
        "cron.* /dev/null\n" },
      { "/etc/net/interfaces",
        "# static since the dhcp lease kept moving us\n"
        "iface eth0\n"
        "  address 10.0.2.15\n"
        "  gateway 10.0.2.2\n" },
      { "/etc/net/interfaces",
        "iface eth0\n"
        "  address 10.0.2.15\n"
        "  gateway 10.0.2.2\n"
        "# mtu lowered for the tunnel, see the runbook\n"
        "  mtu 1400\n" },
      { "/etc/profile",
        "# login shell profile\n"
        "PATH=/bin:/usr/bin:/sbin\n"
        "# added by nomowner: I got tired of typing it\n"
        "alias v=pkg verify\n" },
      { "/etc/profile",
        "# login shell profile\n"
        "PATH=/bin:/usr/bin:/sbin:/usr/sbin\n"
        "# sbin on the path so I stop getting command not found -- nomowner\n" },
      { "/etc/crontab",
        "# nightly log trim, added after we filled the disk in March\n"
        "0 3 * * *  root  rm /var/log/messages\n" },
      { "/etc/ntp.conf",
        "server 10.0.2.4\n"
        "# second source added after the drift complaint\n"
        "server 10.0.2.5\n" },
      { "/etc/httpd/httpd.conf",
        "# port moved off 80, the load balancer terminates now\n"
        "Listen 8080\nDocumentRoot /srv/www\nServerName nominal.local\n" },
      { "/etc/motd",
        "Welcome to NomnixOS.\n"
        "\n"
        "*** dock-2 is scheduled for migration. Do NOT reboot without\n"
        "*** telling ops first. -- J.\n" },
      /* This named /etc/default/postfix, which no package installs, so the
       * edit silently did nothing and one decoy in seventeen was a decoy of a
       * decoy. Postfix reads /etc/postfix/main.cf. */
      { "/etc/postfix/main.cf",
        "myhostname = node.nomnix.org\n"
        "# relay added when we lost direct outbound, 9 Feb\n"
        "relayhost = 10.0.2.7\n" },

      /* ---- and the second batch, added alongside the second generation of
       * faults, because a fault set that doubles and a decoy set that does
       * not turns `pkg verify` back into an oracle: the unfamiliar line is
       * the answer again. Every one of these is a thing a real administrator
       * really does, in a file that really matters, and every one of them
       * leaves a machine that boots with every service running. ---- */

      /* THE ONE THAT TEACHES `nofail`. An fstab entry for a disk that is not
       * in the machine is a fault when it stops the boot and housekeeping
       * when it does not, and the single word that decides which is right
       * there in the options column. It prints on the console at every boot,
       * which is the point: an alarming line that is not the fault. */
      { "/etc/fstab",
        "# device                        mount  type  options\n"
        "UUID=8f41-2c07-a19d-5be3  /      ext4  defaults\n"
        "none                            /proc  proc  defaults\n"
        "none                            /tmp   tmpfs defaults\n"
        "/dev/sr0                        /media iso9660 noauto\n"
        "# the backup caddy is not always in the machine -- nofail, please\n"
        "# leave it, I am tired of retyping it. -- nomowner\n"
        "/dev/sdb1                       /media ext4  nofail\n" },

      /* A vendor tarball, and a path APPENDED rather than prepended -- which
       * is the difference between a working machine and the two-libraries
       * fault, and is invisible unless the order is read. */
      { "/etc/ld.so.conf",
        "/lib\n"
        "/usr/lib\n"
        "# the vendor tools carry their own copies; ours must win, so this\n"
        "# goes LAST. Do not tidy it to the top. -- nomowner\n"
        "/opt/vendor/lib\n" },

      /* An account somebody added. /etc/passwd is the most frightening file
       * on the machine to find changed and this change is completely
       * ordinary. */
      { "/etc/passwd",
        "root:x:0:0:root:/root:/bin/sh\n"
        "daemon:x:1:1:daemon:/:/bin/false\n"
        "nomowner:x:1000:1000:host owner:/home/nomowner:/bin/sh\n"
        "# for the nightly export job, 11 Mar. No shell, on purpose.\n"
        "backup:x:1001:1001:backup agent:/var/backups:/bin/false\n" },

      /* AN EDITED BOOT SCRIPT THAT IS FINE. rc.boot is where a real fault
       * (the vendor `need` line, the left-behind bind) lives, so a harmless
       * edit to it is the most valuable decoy in this list. */
      { "/etc/rc.boot",
        "# /etc/rc.boot -- the bootstrap rc, run by pid 1.\n"
        "# Brings the filesystems online and enters the default runlevel.\n"
        "echo rc.boot: bootstrap rc starting\n"
        "echo rc.boot: site policy 4 applied -- see the runbook\n"
        "need /sbin/svcinit\n"
        "# /etc/fstab is the single source of truth for what gets mounted.\n"
        "exec /sbin/mountall\n"
        "run /etc/rc.d/rc.3\n" },

      { "/etc/inittab",
        "# /etc/inittab -- the last non-comment line is run by /sbin/init.\n"
        "#\n"
        "# Do not add a second command here. init runs the LAST one and the\n"
        "# other is simply ignored, which cost me an afternoon in February.\n"
        "/bin/rc /etc/rc.boot\n" },

      /* The repository file with everything changed EXCEPT the line that
       * matters. Whoever has been burned by the testing channel once will
       * open this file at speed; the channel still says stable. */
      { "/etc/pkg/repos.d/main.repo",
        "# the repository this machine is built from.\n"
        "# switched to the regional mirror 2 May -- the main one was timing\n"
        "# out during the nightly upgrade window.\n"
        "name = main\n"
        "channel = stable\n"
        "url = https://mirror-eu.nomnix.org/11.4\n" },

      { "/etc/logrotate.conf",
        "# daily since the March outage, and keep a fortnight of them\n"
        "daily\nrotate 14\ncompress\ninclude /etc/logrotate.d\n" },

      { "/etc/nomde/panel.conf",
        "# panel moved to the top, the users asked -- 19 Apr\n"
        "position=top\nheight=32\n" },

      { "/etc/nftables.conf",
        "table inet filter {\n"
        "  chain input {\n"
        "    type filter hook input priority 0; policy drop;\n"
        "    # 8080 opened for the new load balancer, ticket 9102\n"
        "    tcp dport { 22, 80, 8080 } accept\n"
        "  }\n}\n" },

      { "/etc/services.d/sshd.svc",
        "# /etc/services.d/sshd.svc\n"
        "name: sshd\n"
        "exec: /usr/sbin/sshd\n"
        "description: remote login\n"
        "after: net\n"
        "# always, not on-failure: ops want it back even after a clean stop\n"
        "restart: always\n"
        "enabled: yes\n"
        "runlevel: 3\n" },

      /* ---- a third batch, alongside the third generation of faults. Same
       * rule as the second: every file that now HOLDS a fault needs a
       * harmless edit of its own, or the fault set has quietly handed
       * `pkg verify` back its oracle. Six of these ten are in files that a
       * fault added in this tranche also writes. ---- */

      /* THE BOOTLOADER CONFIG, EDITED AND FINE. Three faults live in this
       * file now -- a default that is out of range, a second entry, a root
       * named by device -- so an edit to it that is simply somebody being
       * considerate is the most valuable decoy in the list. */
      { "/boot/zbl/zbl.cfg",
        "# timeout raised: the console cart takes twenty seconds to wake up\n"
        "# and I kept missing the menu. -- nomowner\n"
        "default 0\n"
        "timeout 20\n"
        "\n"
        "entry \"NomnixOS 11.4\"\n"
        "  kernel /boot/vmnomuz\n"
        "  initrd /boot/initrd\n"
        "  root UUID=8f41-2c07-a19d-5be3\n" },

      /* /etc/shells matters now: getty checks it. A list somebody has added
       * to is exactly as alarming and exactly as harmless as it looks. */
      { "/etc/shells",
        "/bin/sh\n"
        "/bin/false\n"
        "# the contractors get a restricted shell -- added 6 Feb\n"
        "/bin/rbash\n" },

      /* The runlevel script, which is where the console's account is named.
       * An extra echo, and nothing else. */
      { "/etc/rc.d/rc.3",
        "# /etc/rc.d/rc.3 -- multi-user runlevel.\n"
        "echo rc.3: entering multi-user\n"
        "echo rc.3: site policy 4 applied -- see the runbook\n"
        "exec /sbin/svcinit 3\n"
        "exec /sbin/getty root\n" },

      /* A service unit with a restart policy somebody thought about. */
      { "/etc/services.d/nomde.svc",
        "# /etc/services.d/nomde.svc\n"
        "name: nomde\n"
        "exec: /usr/bin/nomde\n"
        "description: the display server\n"
        "after: net\n"
        "# always: the desk staff complain the moment it is not there\n"
        "restart: always\n"
        "enabled: yes\n"
        "runlevel: 3 5\n" },

      /* The audit trail, tuned. Its config is a bind target in one fault and
       * a truncation target in another. */
      { "/etc/audit/auditd.conf",
        "# 8M was filling every fortnight; the volume has the room\n"
        "log_file = /var/log/audit.log\n"
        "max_log_file = 64\n" },

      { "/etc/crontab",
        "# m h dom mon dow  command\n"
        "17 *  * * *  /usr/sbin/logrotate /etc/logrotate.conf\n"
        "# the package cache never gets cleaned otherwise -- 2 June\n"
        "30 4  * * 0  root  rm /var/cache/nomnix-1400.pkg\n" },

      { "/etc/httpd/httpd.conf",
        "Listen 80\n"
        "DocumentRoot /srv/www\n"
        "# renamed for the new certificate, 8 May\n"
        "ServerName node.nomnix.org\n" },

      { "/etc/nsswitch.conf",
        "# files FIRST, deliberately: the entries in /etc/hosts are the\n"
        "# authority on this network and dns is a second opinion.\n"
        "passwd: files\n"
        "group: files\n"
        "hosts: files dns\n" },

      { "/etc/pkg/pkg.conf",
        "# how aggressive upgrades are allowed to be\n"
        "# downgrades turned on for the rollback in March and left on,\n"
        "# because we will need it again. -- nomowner\n"
        "allow_downgrade = yes\n"
        "check_signatures = yes\n" },

      { "/etc/logrotate.d/syslog",
        "/var/log/messages {\n"
        "  daily\n"
        "  rotate 30\n"
        "  # a month of them since the March outage -- ops asked\n"
        "}\n" },
    };
    const int NEDITS = (int)(sizeof EDITS / sizeof EDITS[0]);

    /* NOM_FORCE_EDIT=<n>: install exactly decoy n and nothing else. */
    const char *fe = getenv("NOM_FORCE_EDIT");
    if (fe) {
        int i = atoi(fe) % NEDITS;
        VNode *n = vfs_lookup(&m->disk, EDITS[i].path);
        if (n && n->kind == VN_FILE) {
            buf_clear(&n->data);
            buf_puts(&n->data, EDITS[i].content);
            record_local(m, EDITS[i].path, EDITS[i].content);
        }
        return;
    }

    /* Two to five of them, chosen by the seed. More than before, because the
     * point is that `pkg verify` output has to be READ rather than skimmed
     * for the one familiar line. Duplicates by path are skipped, so a machine
     * never gets two versions of the same file. */
    /* DEALT ROUND, NOT ROLLED, for the same reason the faults are.
     *
     * Independent draws gave the right per-decoy rate and the wrong session:
     * a blind playtester reported that `Listen 8080` in httpd.conf and the
     * `restart: always` note in nomde.svc "appeared on nearly every machine",
     * and stopped reading decoys at all. They were not imagining it -- with
     * three or four drawn independently out of thirty-seven, the same file
     * turns up on consecutive machines about as often as not, and two
     * machines in a row is all it takes to file something under scenery. A
     * decoy that is always there is not a decoy.
     *
     * So each machine takes a run of the table starting where the seed says,
     * and the stride is coprime with its length, which spreads the run out
     * and puts a different set in front of the player on the next call. */
    int want = 2 + (int)(rng_next(&r) % 4);
    Rng pr;
    rng_seed(&pr, (seed / (uint64_t)NEDITS) ^ 0x6d2b79f5a3c1e94bULL);
    int at = (int)((seed + rng_next(&pr)) % (uint64_t)NEDITS);
    for (int k = 0; k < want && m->nlocal < 8; k++, at = (at + 6) % NEDITS) {
        int i = at;
        bool dup = false;
        for (int j = 0; j < m->nlocal; j++)
            if (strcmp(m->local[j], EDITS[i].path) == 0) dup = true;
        if (dup) continue;
        VNode *n = vfs_lookup(&m->disk, EDITS[i].path);
        if (!n || n->kind != VN_FILE) continue;
        buf_clear(&n->data);
        buf_puts(&n->data, EDITS[i].content);
        record_local(m, EDITS[i].path, EDITS[i].content);
    }
}

/* Did the repair survive the administrator's decisions?
 *
 * A playtester's sharpest criticism was that nothing stopped them reinstalling
 * every flagged package, and there was no cost to being sloppy. There is one
 * now: `pkg reinstall` keeps modified config unless forced, and this reports
 * what was reverted anyway. Fixing the machine while quietly undoing
 * somebody's work is not the same as fixing the machine.
 *
 * WHAT IT USED TO COMPARE, AND WHY THAT WAS THE OPPOSITE OF WHAT IT SAID.
 *
 * It held the whole file as it stood when the player arrived and fired if a
 * single byte of it had changed since. That is not "did you clobber local
 * config", it is "did you edit this file at all" -- and the files carrying a
 * local edit are, by design, the same files the faults live in. So the
 * surgical repair the game teaches was the thing it punished. A player on
 * seed 8129 changed one word in the fstab line for the optical drive, left
 * the customer's hand-written `/dev/sdb1 nofail` lines and their comment
 * exactly where they were, confirmed with `pkg diff` that those lines were
 * the only remaining delta, and was told at the login prompt:
 *
 *   you overwrote local configuration:
 *     /etc/fstab
 *     this machine's own settings are gone and there is no undo.
 *
 * Nothing local was lost. And the counter-case ran the other way: a player
 * who edited /etc/passwd until `pkg diff` said "identical -- nothing to fix
 * here" had genuinely erased whatever was local in it, and the check was
 * silent, because on that seed passwd carried no seeded edit at all. It
 * rewarded the destructive repair and accused the correct one, on the exact
 * mechanic notes.txt hint 4 teaches.
 *
 * The local decision was never the file. It is the handful of LINES the
 * package does not ship -- the caddy, the second nameserver, the backup
 * account, the vendor path that has to go last. Those are what somebody chose
 * on purpose, those are what a forced reinstall takes away, and those are
 * what this asks about now. Change the uuid on the root line and every local
 * line is still there. Restore the file to what shipped and every one of them
 * is gone, which is the sentence this has been printing all along.
 *
 * It names the lines, because "/etc/fstab" alone left the player hunting for
 * what they were supposed to have destroyed. */
int machine_collateral(Machine *m, Buf *out)
{
    int lost = 0;
    for (int i = 0; i < m->nlocal; i++) {
        VNode *n = vfs_lookup(&m->disk, m->local[i]);
        bool isfile = n && n->kind == VN_FILE;
        const Buf *keep = &m->local_orig[i];
        if (!keep->len) continue;      /* nothing local left to lose */

        Buf now = {0};
        if (isfile && n->data.len) buf_put(&now, n->data.p, n->data.len);
        int missing = 0;
        const char *p = keep->p, *end = keep->p + keep->len;
        Buf which = {0};
        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (!line_present(&now, p, ll)) {
                if (missing < 3) {
                    buf_puts(&which, "      ");
                    buf_put(&which, p, ll);
                    buf_putc(&which, '\n');
                }
                missing++;
            }
            p = nl ? nl + 1 : end;
        }
        buf_free(&now);
        if (missing) {
            if (!lost) buf_puts(out, "\nyou overwrote local configuration:\n");
            buf_printf(out, "  %s -- %d line(s) somebody put there are gone:\n",
                       m->local[i], missing);
            buf_put(out, which.p, which.len);
            if (missing > 3) buf_printf(out, "      ... and %d more\n", missing - 3);
            lost++;
        }
        buf_free(&which);
    }
    if (lost)
        buf_puts(out,
            /* "there is no undo" was FALSE, and provably so one line
             * earlier: pkg prints "saved your /etc/hosts as
             * /etc/hosts.pkgsave" as it goes. A playtester restored the file
             * with cp and reported the message as a lie, correctly. The point
             * stands without the exaggeration -- the copy is one command
             * away, and nobody looks for it unless told it is there. */
            "  this machine's own settings are gone. `pkg` saved the old file\n"
            "  beside the new one -- `ls /etc/*.pkgsave` -- and `cp` puts it back.\n"
            "  somebody chose them on purpose; `pkg diff` shows what a file\n"
            "  says against what the package ships, and plain `pkg reinstall`\n"
            "  (without --force) leaves edited config alone.\n");
    return lost;
}

/* Re-baseline the local edits against the disk AS THE PLAYER RECEIVES IT.
 *
 * The collateral report asks one question: did YOU destroy something that was
 * there when you arrived. It compared against the edits as INSTALLED, which is
 * a different question -- if the breaker then corrupted one of those files,
 * the report fired before the player had typed a single command, blaming them
 * for damage the ticket shipped with. A playtester spent twenty minutes trying
 * to work out what it meant and concluded, reasonably, that the same message
 * means both "you broke this" and "the customer broke this".
 *
 * Same question, now asked line by line: a local line the BREAKER already
 * removed is not the player's to lose, so it stops being watched. What is
 * left is exactly the set of local decisions that were on the disk at the
 * moment it was handed over.
 *
 * Called once the machine is broken and before anyone touches it. */
void machine_rebaseline_local(Machine *m)
{
    for (int i = 0; i < m->nlocal; i++) {
        VNode *n = vfs_lookup(&m->disk, m->local[i]);
        Buf now = {0};
        if (n && n->kind == VN_FILE && n->data.len)
            buf_put(&now, n->data.p, n->data.len);
        Buf keep = {0};
        const char *p = m->local_orig[i].p;
        const char *end = p ? p + m->local_orig[i].len : NULL;
        while (p && p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (line_present(&now, p, ll)) {
                buf_put(&keep, p, ll);
                buf_putc(&keep, '\n');
            }
            p = nl ? nl + 1 : end;
        }
        buf_clear(&m->local_orig[i]);
        if (keep.len) buf_put(&m->local_orig[i], keep.p, keep.len);
        buf_free(&keep);
        buf_free(&now);
    }
}

/* DAMAGE THAT IS STILL THERE, whether or not the machine boots.
 *
 * A playtester took a three-fault ticket, repaired two, booted, and was told
 * [UP at target] with no complaint -- while /etc/udev/rules.d/50-default.rules
 * still read SUBSYSTEM=="bhock". The health check only ever asked whether the
 * services were running, so a fault that has not broken anything YET signs off
 * as fixed. That undercuts the whole premise: the ticket is "prove it is
 * healthy", not "prove it starts today".
 *
 * A package file that no longer matches what the package ships, and that is
 * not one of this machine's own local edits, is outstanding damage. That
 * definition needs no cooperation from the faults and no list of what was
 * injected -- it is just the truth about the disk.
 */
/* THERE WAS NO WAY TO FINISH A JOB.
 *
 * A blind playtester repaired seven machines and wrote: "The customer never
 * says it's working, no ticket is marked resolved... `[UP at target]` is the
 * entire payoff and it's easy to miss." Worse, `rcon power cycle` does not
 * print that line at all, so their first fully repaired ticket ended in
 * silence. A shift made of jobs that never end is not a shift.
 *
 * `done` is a CLAIM the game checks, not a button that congratulates you.
 * Signing off a machine that is still broken is the mistake this job really
 * punishes, so the refusal reports what the customer can see and leaves the
 * diagnosis where it belongs. A machine sitting on the rescue medium is not
 * repaired however healthy it looks -- that image was never broken.
 */
bool machine_handback(Machine *m, Buf *out)
{
    /* HANDING A MACHINE BACK DOES NOT TOUCH THE MACHINE.
     *
     * This used to boot it first, and a reboot is a REPAIR: it is the one
     * thing that fixes a daemon running a configuration nobody reloaded,
     * because the daemon reads the file again on the way up. So the ticket
     * whose whole lesson is `kill -HUP` could be closed by typing `done`, and
     * a blind playtester found seed 1234 doing exactly that -- two commands,
     * no repair, "ticket closed", after they had spent twenty minutes
     * correctly proving the machine was healthy. It was healthy: the verdict
     * had quietly fixed it before judging it. That teaches a player to
     * distrust their own diagnosis, which is the one thing this game exists
     * to build.
     *
     * It also meant the rescue-medium branch below could never fire, since
     * booting the disk clears on_rescue on the way past.
     *
     * A claim is checked against the machine AS IT STANDS. If the last thing
     * it did was fail to boot, that is the answer, and the refusal says so.
     */
    Buf sick = {0};
    int dead = kernel_health(m, &sick);
    Buf left = {0}, lost = {0};
    int rest = machine_outstanding(m, &left) ? 1 : 0;
    bool closed = false;

    buf_puts(out, "you tell them it is fixed.\n\n");
    if (!m->boot.running) {
        buf_puts(out,
            "  \"...it is still doing the same thing. It has not come up.\"\n\n"
            "it does not reach a login prompt. `boot` and read what it says on\n"
            "the way down.\n");
    } else if (m->on_rescue) {
        buf_puts(out,
            "  \"There is a prompt, but it is not asking for my name like it\n"
            "   normally does. And my files are not where I left them.\"\n\n"
            "that is the rescue medium, not their system -- a separate working\n"
            "install that was never broken, so of course it boots. put their own\n"
            "disk back in front of them:\n"
            "  rcon media eject / rcon boot disk / rcon power cycle\n");
    } else if (dead || rest) {
        buf_puts(out,
            "  \"It starts up now, thank you -- but it is still not right.\"\n\n");
        if (dead && sick.len) buf_put(out, sick.p, sick.len);
        if (rest && left.len) buf_put(out, left.p, left.len);
    } else if (machine_collateral(m, &lost) && lost.len) {
        /* THE BOOT KNEW AND THE VERDICT DID NOT.
         *
         * A playtester --force'd over /etc/hosts, watched the machine name the
         * three lines they had just destroyed, and then handed the machine
         * back one command later to a closing message asserting that nothing
         * differed "except what somebody meant to change". The warning was
         * wired to `boot`; the verdict never asked.
         *
         * It STILL CLOSES, and that is deliberate. The machine is repaired --
         * it boots from its own disk and every service is up -- and refusing
         * to close would be answering a different question from the one asked.
         * (I tried refusing first. It also made every ticket unclosable,
         * because the repair ladder force-reinstalls everything by design, and
         * the gate said 0 of 60 immediately.) What was wrong was never the
         * verdict; it was a closing message that asserted the absence of the
         * exact damage the player had done. */
        buf_puts(out,
            "  \"That is it running again. Thank you.\"\n\n");
        buf_printf(out, "--- ticket %s closed ---\n", m->id);
        buf_puts(out,
            "  it boots from its own disk and every service that should be\n"
            "  running is running. you also took something with you:\n\n");
        buf_put(out, lost.p, lost.len);
        buf_puts(out,
            "\nnothing will fail today. it will fail the day somebody needs that\n"
            "line, and by then this call is closed. `pkg` saved the old file\n"
            "beside the new one -- `ls /etc/*.pkgsave` -- and `cp` puts it back.\n\n"
            "`ticket` takes the next call.\n");
        closed = true;
    } else {
        buf_puts(out,
            "  \"Oh, that is it -- that is exactly how it looked before.\n"
            "   Thank you. I will let you get on.\"\n\n");
        buf_printf(out, "--- ticket %s closed ---\n", m->id);
        buf_puts(out,
            "  it boots from its own disk, every service that should be running\n"
            "  is running, and nothing on it differs from what its packages\n"
            "  shipped except what somebody meant to change.\n\n"
            "`ticket` takes the next call.\n");
        closed = true;
    }
    buf_free(&sick);
    buf_free(&left);
    buf_free(&lost);
    return closed;
}


int machine_outstanding(Machine *m, Buf *out)
{
    int bad = 0;
    for (int i = 0; i < m->npkg; i++) {
        const Package *p = m->pkg[i];
        for (int j = 0; j < p->nfiles; j++) {
            const PkgFile *f = &p->file[j];
            if (f->isdir || f->link) continue;

            bool is_local = false;
            for (int k = 0; k < m->nlocal; k++)
                if (strcmp(m->local[k], f->path) == 0) is_local = true;
            if (is_local) continue;

            /* A FILE THE SANCTIONED REPAIR TOOL REGENERATES IS NOT DAMAGE.
             *
             * `mkinitrd` builds an initrd from the modules on THIS machine and
             * `zbl-mkconfig` writes a bootloader config from the kernels it
             * finds -- that is what they are for, and it is the documented fix
             * for several faults. Their output cannot be byte-identical to
             * what the package shipped, and nothing is wrong with that.
             *
             * This flagged the result anyway, so a machine repaired exactly as
             * the game teaches came back "still not as its package shipped
             * it", naming /boot/initrd-6.4.11. Every one of sixty repaired
             * tickets was refused a hand-back for this reason. Telling a
             * player their correct repair was vandalism is the same lie as
             * the console faking a boot, in a quieter voice. */
            if (strncmp(f->path, "/boot/initrd", 12) == 0 ||
                strcmp(f->path, "/boot/zbl/zbl.cfg") == 0) continue;

            VNode *n = vfs_lookup(&m->disk, f->path);
            Buf want = {0};
            pristine(m, f, &want);
            bool differs = !n || n->kind != VN_FILE ||
                           n->data.len != want.len ||
                           (want.len && memcmp(n->data.p, want.p, want.len) != 0);
            buf_free(&want);
            if (!differs) continue;

            if (!bad) buf_puts(out,
                "\nthe machine is up, but this is still not as its package "
                "shipped it:\n");
            if (bad < 6) buf_printf(out, "  %s  (%s)\n", f->path, p->name);
            bad++;
        }
    }
    if (bad) buf_puts(out,
        "  a fault that has not broken anything yet is still a fault --\n"
        "  `pkg diff <path>` to see it.\n");
    return bad;
}

/* Take the box off the network on the way out. Without this every finished
 * ticket left a machine holding a switch port and a DHCP lease it was never
 * going to give back, so the twenty-fifth ticket in a run found the switch
 * full -- and what a ticket's network looked like depended on how many had
 * run before it, which is a seed that stops reproducing. See netsite.c. */
void netsite_detach(Machine *m);

void machine_free(Machine *m)
{
    netsite_detach(m);
    for (int i = 0; i < 8; i++) buf_free(&m->local_orig[i]);
    kernel_stop_daemons(m);
    vfs_free(&m->disk);
    vfs_free(&m->rescue);
    buf_free(&m->boot.console);
    /* What the customer is looking at is a copy of what the machine printed,
     * and it is hers until the call ends. */
    buf_free(&m->cust.screen);
}

/* --- the package database -------------------------------------------- */

const Package *pkg_find(const Machine *m, const char *name)
{
    for (int i = 0; i < m->npkg; i++)
        if (strcmp(m->pkg[i]->name, name) == 0) return m->pkg[i];
    return NULL;
}

const Package *pkg_owns(const Machine *m, const char *path)
{
    for (int i = 0; i < m->npkg; i++)
        for (int j = 0; j < m->pkg[i]->nfiles; j++)
            if (strcmp(m->pkg[i]->file[j].path, path) == 0) return m->pkg[i];
    return NULL;
}

static void pristine(const Machine *m, const PkgFile *f, Buf *out)
{
    if (f->content) buf_puts(out, f->content);
    else            image_generated(m, f->path, out);
}

static void verify_pkg(Machine *m, const Package *p, Buf *out, int *bad)
{
    for (int j = 0; j < p->nfiles; j++) {
        const PkgFile *f = &p->file[j];
        VNode *n = vfs_lookup(&m->disk, f->path);
        if (!n) { buf_printf(out, "%s missing\n", f->path); (*bad)++; continue; }
        if (f->isdir) {
            if (n->kind != VN_DIR)       { buf_printf(out, "%s changed\n", f->path); (*bad)++; }
            else if (n->mode != f->mode) { buf_printf(out, "%s mode\n",    f->path); (*bad)++; }
            continue;
        }
        if (f->link) {
            if (n->kind != VN_LINK || strcmp(n->target, f->link) != 0) {
                buf_printf(out, "%s changed\n", f->path); (*bad)++;
            }
            continue;
        }
        if (n->kind != VN_FILE) { buf_printf(out, "%s changed\n", f->path); (*bad)++; continue; }
        Buf want = {0};
        pristine(m, f, &want);
        bool differs = (want.len != n->data.len) ||
                       (want.len && memcmp(want.p, n->data.p, want.len) != 0);
        buf_free(&want);
        if (differs)                 { buf_printf(out, "%s changed\n", f->path); (*bad)++; }
        else if (n->mode != f->mode) { buf_printf(out, "%s mode\n",    f->path); (*bad)++; }
    }
}

void pkg_verify(Machine *m, const char *name, Buf *out)
{
    int bad = 0;
    if (name) {
        const Package *p = pkg_find(m, name);
        if (!p) { buf_printf(out, "no such package: %s\n", name); return; }
        verify_pkg(m, p, out, &bad);
    } else {
        for (int i = 0; i < m->npkg; i++) verify_pkg(m, m->pkg[i], out, &bad);
    }
    if (bad == 0) buf_puts(out, "all files match their packages\n");
}

bool pkg_file_content(const Machine *m, const char *pkgname, const char *path,
                      Buf *out)
{
    const Package *p = pkg_find(m, pkgname);
    if (!p) return false;

    /* THE CHANNEL DECIDES WHAT THE REPOSITORY SERVES. On `testing` the libc
     * is 12.0's, which nothing on this machine is linked against -- so an
     * upgrade from the wrong channel installs a perfectly valid library that
     * every binary refuses to run with. The fault is the config, not the
     * file, and `pkg verify` will happily report the file as wrong when the
     * real problem is where it came from. */
    if (m->channel[0] && strcmp(m->channel, "stable") != 0) {
        if (strcmp(path, "/lib/libc.so.6") == 0) {
            buf_puts(out, "stub libc 2.41\n");
            return true;
        }
        if (strcmp(path, "/lib/libm.so.6") == 0) {
            buf_puts(out, "stub libm 2.41\n");
            return true;
        }
    }
    for (int j = 0; j < p->nfiles; j++) {
        if (strcmp(p->file[j].path, path) != 0) continue;
        if (p->file[j].isdir || p->file[j].link) {
            /* A directory and a symlink both have no bytes to hand back. They
             * are RESTORED by pkg_restore_path, never here.
             *
             * This function used to do the restoring itself, which made it a
             * read that wrote: `pkg diff /boot/vmnomuz` on a dangling symlink
             * silently repaired the symlink and solved the machine for the
             * player. A blind playtester hit exactly that, watched `ls` show
             * a healthy link one command after `stat` said the path did not
             * exist, and reasonably called it the worst bug in the game. */
            return true;
        }
        pristine(m, &p->file[j], out);
        return true;
    }
    return false;
}

/* Put one path back the way the package ships it. The MUTATING counterpart of
 * pkg_file_content, kept separate so that fetching a file can never change the
 * machine -- `pkg diff` reads, `pkg reinstall` writes, and the two must not
 * share a code path that does both. */
bool pkg_restore_path(Machine *m, const char *pkgname, const char *path)
{
    const Package *p = pkg_find(m, pkgname);
    if (!p) return false;
    for (int j = 0; j < p->nfiles; j++) {
        if (strcmp(p->file[j].path, path) != 0) continue;
        if (p->file[j].isdir) {
            /* SOMETHING ELSE IN THE WAY IS NOT A DIRECTORY THAT IS THERE.
             *
             * vfs_mkdir walks to an existing node and hands it back whatever
             * kind it is, so a package whose directory had been replaced by a
             * FILE -- an archive unpacked one level too high, which is a real
             * afternoon -- reinstalled "successfully" and left the file
             * exactly where it was. The solve ladder scored it unfixable and
             * it was right to. rpm removes what is in the way; so does this. */
            VNode *ex = vfs_lookup(&m->disk, path);
            if (ex && ex->kind != VN_DIR) vfs_remove(&m->disk, path);
            VNode *d = vfs_mkdir(&m->disk, path);
            if (d) d->mode = p->file[j].mode;
            return true;
        }
        if (p->file[j].link) {
            vfs_remove(&m->disk, path);
            vfs_symlink(&m->disk, p->file[j].link, path);
            return true;
        }
        vfs_remove(&m->disk, path);
        install_file(m, &p->file[j]);
        return true;
    }
    return false;
}

int pkg_reinstall(Machine *m, const char *name, Buf *out)
{
    const Package *p = pkg_find(m, name);
    if (!p) { buf_printf(out, "no such package: %s\n", name); return 0; }
    int n = 0;
    for (int j = 0; j < p->nfiles; j++) {
        vfs_remove(&m->disk, p->file[j].path);
        install_file(m, &p->file[j]);
        n++;
    }
    buf_printf(out, "%s-%s: %d files restored\n", p->name, p->version, n);
    return n;
}
