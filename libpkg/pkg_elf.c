/*-
 * Copyright (c) 2011-2024 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2012-2013 Matthew Seaman <matthew@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef HAVE_CONFIG_H
#include "pkg_config.h"
#endif

#ifdef HAVE_SYS_ENDIAN_H
#include <sys/endian.h>
#elif HAVE_ENDIAN_H
#include <endian.h>
#elif HAVE_MACHINE_ENDIAN_H
#include <machine/endian.h>
#endif
#include <sys/types.h>
#if defined(HAVE_SYS_ELF_COMMON_H) && !defined(__DragonFly__)
#include <sys/elf_common.h>
#endif
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <gelf.h>
#include <libgen.h>
#if defined(HAVE_LINK_H) && !defined(__DragonFly__) && defined(HAVE_LIBELF)
#include <link.h>
#endif
#include <paths.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_LIBELF
#include <libelf.h>
#endif

#include <bsd_compat.h>

#include "pkg.h"
#include "private/pkg.h"
#include "private/event.h"
#include "private/elf_tables.h"
#include "private/ldconfig.h"
#include "private/binfmt.h"

#ifndef NT_ABI_TAG
#define NT_ABI_TAG 1
#endif


/* FFR: when we support installing a 32bit package on a 64bit host */
#define _PATH_ELF32_HINTS       "/var/run/ld-elf32.so.hints"

#ifndef roundup2
#define roundup2(x, y)	(((x)+((y)-1))&(~((y)-1))) /* if y is powers of two */
#endif

static const char * elf_corres_to_string(const struct _elf_corres* m, int e);
static int elf_string_to_corres(const struct _elf_corres* m, const char *s);

static int
filter_system_shlibs(const char *name, char *path, size_t pathlen)
{
	const char *shlib_path;

	shlib_path = shlib_list_find_by_name(name);
	if (shlib_path == NULL) {
		/* dynamic linker could not resolve */
		return (EPKG_FATAL);
	}

	if (pkg_object_bool(pkg_config_get("ALLOW_BASE_SHLIBS"))) {
		if (strstr(shlib_path, "/lib32/") != NULL)
			return (EPKG_END);
	} else {
		/* match /lib, /lib32, /usr/lib and /usr/lib32 */
		if (strncmp(shlib_path, "/lib", 4) == 0 ||
		    strncmp(shlib_path, "/usr/lib", 8) == 0)
			return (EPKG_END); /* ignore libs from base */
	}

	if (path != NULL)
		strncpy(path, shlib_path, pathlen);

	return (EPKG_OK);
}

/* ARGSUSED */
static int
add_shlibs_to_pkg(struct pkg *pkg, const char *fpath, const char *name,
    bool is_shlib)
{
	struct pkg_file *file = NULL;
	const char *filepath;
	size_t fsz, nsz;

	switch(filter_system_shlibs(name, NULL, 0)) {
	case EPKG_OK:		/* A non-system library */
		pkg_addshlib_required(pkg, name);
		return (EPKG_OK);
	case EPKG_END:		/* A system library */
		return (EPKG_OK);
	default:
		/* Ignore link resolution errors if we're analysing a
		   shared library. */
		if (is_shlib)
			return (EPKG_OK);

		while (pkg_files(pkg, &file) == EPKG_OK) {
			filepath = file->path;
			fsz = strlen(filepath);
			nsz = strlen(name);

			if (fsz >= nsz &&
			    STREQ(&filepath[fsz - nsz], name)) {
				pkg_addshlib_required(pkg, name);
				return (EPKG_OK);
			}
		}

		pkg_emit_notice("(%s-%s) %s - required shared library %s not "
		    "found", pkg->name, pkg->version, fpath, name);

		return (EPKG_FATAL);
	}
}

static bool
shlib_valid_abi(const char *fpath, GElf_Ehdr *hdr)
{
	int semicolon;
	const char *p, *t;
	char arch[64], wordsize[64];
	int wclass;
	const char *shlib_arch;

	/*
	 * ALTABI string is in format:
	 * <osname>:<osversion>:<arch>:<wordsize>[.other]
	 * We need here arch and wordsize only
	 */
	arch[0] = '\0';
	wordsize[0] = '\0';
	p = pkg_object_string(pkg_config_get("ABI"));
	for(semicolon = 0; semicolon < 3 && p != NULL; semicolon ++, p ++) {
		p = strchr(p, ':');
		if (p != NULL) {
			switch(semicolon) {
			case 1:
				/* We have arch here */
				t = strchr(p + 1, ':');
				/* Abi line is likely invalid */
				if (t == NULL)
					return (true);
				strlcpy(arch, p + 1, MIN((long)sizeof(arch), t - p));
				break;
			case 2:
				t = strchr(p + 1, ':');
				if (t == NULL)
					strlcpy(wordsize, p + 1, sizeof(wordsize));
				else
					strlcpy(wordsize, p + 1, MIN((long)sizeof(wordsize), t - p));
				break;
			}
		}
	}
	/* Invalid ABI line */
	if (arch[0] == '\0' || wordsize[0] == '\0')
		return (true);

	shlib_arch = elf_corres_to_string(mach_corres, (int)hdr->e_machine);
	if (shlib_arch == NULL)
		return (true);

	wclass = elf_string_to_corres(wordsize_corres, wordsize);
	if (wclass == -1)
		return (true);


	/*
	 * Compare wordsize first as the arch for amd64/i386 is an abmiguous
	 * 'x86'
	 */
	if ((int)hdr->e_ident[EI_CLASS] != wclass) {
		pkg_debug(1, "not valid elf class for shlib: %s: %s",
		    elf_corres_to_string(wordsize_corres,
		    (int)hdr->e_ident[EI_CLASS]),
		    fpath);
		return (false);
	}

	if (!STREQ(shlib_arch, arch)) {
		pkg_debug(1, "not valid abi for shlib: %s: %s", shlib_arch,
		    fpath);
		return (false);
	}

	return (true);
}

#ifdef __FreeBSD__
static bool
is_old_freebsd_armheader(const GElf_Ehdr *e)
{
	GElf_Word eabi;

	/*
	 * Old FreeBSD arm EABI binaries were created with zeroes in [EI_OSABI].
	 * Attempt to identify them by the little bit of valid info that is
	 * present:  32-bit ARM with EABI version 4 or 5 in the flags.  OABI
	 * binaries (prior to freebsd 10) have the correct [EI_OSABI] value.
	 */
	if (e->e_machine == EM_ARM && e->e_ident[EI_CLASS] == ELFCLASS32) {
		eabi = e->e_flags & 0xff000000;
		if (eabi == 0x04000000 || eabi == 0x05000000)
			return (true);
	}
	return (false);
}
#endif

#ifndef HAVE_ELF_NOTE
typedef Elf32_Nhdr Elf_Note;
#endif

static int
analyse_elf(struct pkg *pkg, const char *fpath)
{
	Elf *e = NULL;
	GElf_Ehdr elfhdr;
	Elf_Scn *scn = NULL;
	Elf_Scn *note = NULL;
	Elf_Scn *dynamic = NULL;
	GElf_Shdr shdr;
	Elf_Data *data;
	GElf_Dyn *dyn, dyn_mem;
	struct stat sb;
	int ret = EPKG_OK;

	size_t numdyn = 0;
	size_t sh_link = 0;
	size_t dynidx;
	const char *shlib;
	char *rpath = NULL;

	bool is_shlib = false;

	int fd;

	pkg_debug(1, "analysing elf %s", fpath);
	if (lstat(fpath, &sb) != 0)
		pkg_emit_errno("fstat() failed for", fpath);
	/* ignore empty files and non regular files */
	if (sb.st_size == 0 || !S_ISREG(sb.st_mode))
		return (EPKG_END); /* Empty file or sym-link: no results */

	if ((fd = open(fpath, O_RDONLY, 0)) < 0) {
		return (EPKG_FATAL);
	}

	if (elf_version(EV_CURRENT) == EV_NONE) {
		pkg_emit_error("ELF library initialization failed: %s",
		    elf_errmsg(-1));
		return (EPKG_FATAL);
	}

	if ((e = elf_begin(fd, ELF_C_READ, NULL)) == NULL) {
		ret = EPKG_FATAL;
		pkg_debug(1, "elf_begin() for %s failed: %s", fpath,
		    elf_errmsg(-1));
		goto cleanup;
	}

	if (elf_kind(e) != ELF_K_ELF) {
		/* Not an elf file: no results */
		ret = EPKG_END;
		pkg_debug(1, "not an elf");
		goto cleanup;
	}

	if (ctx.developer_mode)
		pkg->flags |= PKG_CONTAINS_ELF_OBJECTS;

	if (gelf_getehdr(e, &elfhdr) == NULL) {
		ret = EPKG_WARN;
		pkg_debug(1, "getehdr() failed: %s.", elf_errmsg(-1));
		goto cleanup;
	}

	if (elfhdr.e_type != ET_DYN && elfhdr.e_type != ET_EXEC &&
	    elfhdr.e_type != ET_REL) {
		pkg_debug(1, "not an elf");
		ret = EPKG_END;
		goto cleanup;
	}

	/* Elf file has sections header */
	while ((scn = elf_nextscn(e, scn)) != NULL) {
		if (gelf_getshdr(scn, &shdr) != &shdr) {
			ret = EPKG_FATAL;
			pkg_emit_error("getshdr() for %s failed: %s", fpath,
					elf_errmsg(-1));
			goto cleanup;
		}
		switch (shdr.sh_type) {
		case SHT_NOTE:
			if ((data = elf_getdata(scn, NULL)) == NULL) {
				ret = EPKG_END; /* Some error occurred, ignore this file */
				goto cleanup;
			}
			else if (data->d_buf != NULL) {
				Elf_Note *en = (Elf_Note *)data->d_buf;
				if (en->n_type == NT_ABI_TAG)
					note = scn;
			}
			break;
		case SHT_DYNAMIC:
			dynamic = scn;
			sh_link = shdr.sh_link;
			if (shdr.sh_entsize == 0) {
				ret = EPKG_END;
				goto cleanup;
			}
			numdyn = shdr.sh_size / shdr.sh_entsize;
			break;
		}

		if (note != NULL && dynamic != NULL)
			break;
	}

	/*
	 * note == NULL usually means a shared object for use with dlopen(3)
	 * dynamic == NULL means not a dynamically linked elf
	 */
	if (dynamic == NULL) {
		ret = EPKG_END;
		goto cleanup; /* not a dynamically linked elf: no results */
	}

	if (!shlib_valid_abi(fpath, &elfhdr)) {
		ret = EPKG_END;
		goto cleanup; /* Invalid ABI */
	}

#ifdef __FreeBSD__
	if (elfhdr.e_ident[EI_OSABI] != ELFOSABI_FREEBSD &&
	    !is_old_freebsd_armheader(&elfhdr)) {
		ret = EPKG_END;
		goto cleanup;
	}
#endif

	if ((data = elf_getdata(dynamic, NULL)) == NULL) {
		ret = EPKG_END; /* Some error occurred, ignore this file */
		goto cleanup;
	}

	/* First, scan through the data from the .dynamic section to
	   find any RPATH or RUNPATH settings.  These are colon
	   separated paths to prepend to the ld.so search paths from
	   the ELF hints file.  These always seem to come right after
	   the NEEDED shared library entries.

	   NEEDED entries should resolve to a filename for installed
	   executables, but need not resolve for installed shared
	   libraries -- additional info from the apps that link
	   against them would be required.  Shared libraries are
	   distinguished by a DT_SONAME tag */

	rpath_list_init();
	for (dynidx = 0; dynidx < numdyn; dynidx++) {
		if ((dyn = gelf_getdyn(data, dynidx, &dyn_mem)) == NULL) {
			ret = EPKG_FATAL;
			pkg_emit_error("getdyn() failed for %s: %s", fpath,
			    elf_errmsg(-1));
			goto cleanup;
		}

		if (dyn->d_tag == DT_SONAME) {
			is_shlib = true;

			/* The file being scanned is a shared library
			   *provided* by the package. Record this if
			   appropriate */
			shlib = elf_strptr(e, sh_link, dyn->d_un.d_val);
			if (shlib != NULL && *shlib != '\0')
				pkg_addshlib_provided(pkg, shlib);
		}

		if ((dyn->d_tag == DT_RPATH || dyn->d_tag == DT_RUNPATH) &&
		    rpath == NULL)
			rpath = elf_strptr(e, sh_link, dyn->d_un.d_val);
	}
	if (rpath != NULL) {
		char *p = xstrdup(fpath);
		shlib_list_from_rpath(rpath, get_dirname(p));
		free(p);
	}

	/* Now find all of the NEEDED shared libraries. */

	for (dynidx = 0; dynidx < numdyn; dynidx++) {
		if ((dyn = gelf_getdyn(data, dynidx, &dyn_mem)) == NULL) {
			ret = EPKG_FATAL;
			pkg_emit_error("getdyn() failed for %s: %s", fpath,
			    elf_errmsg(-1));
			goto cleanup;
		}

		if (dyn->d_tag != DT_NEEDED)
			continue;

		shlib = elf_strptr(e, sh_link, dyn->d_un.d_val);

		add_shlibs_to_pkg(pkg, fpath, shlib, is_shlib);
	}

cleanup:
	rpath_list_free();

	if (e != NULL)
		elf_end(e);
	close(fd);

	return (ret);
}

static int
analyse_fpath(struct pkg *pkg, const char *fpath)
{
	const char *dot;

	dot = strrchr(fpath, '.');

	if (dot == NULL)	/* No extension */
		return (EPKG_OK);

	if (dot[1] == 'a' && dot[2] == '\0')
		pkg->flags |= PKG_CONTAINS_STATIC_LIBS;

	if ((dot[1] == 'l' && dot[2] == 'a' && dot[3] == '\0'))
		pkg->flags |= PKG_CONTAINS_LA;

	return (EPKG_OK);
}

static const char *
elf_corres_to_string(const struct _elf_corres* m, int e)
{
	int i = 0;

	for (i = 0; m[i].string != NULL; i++)
		if (m[i].elf_nb == e)
			return (m[i].string);

	return ("unknown");
}

static int
elf_string_to_corres(const struct _elf_corres* m, const char *s)
{
	int i = 0;

	for (i = 0; m[i].string != NULL; i++)
		if (STREQ(m[i].string, s))
			return (m[i].elf_nb);

	return (-1);
}

static const char *
aeabi_parse_arm_attributes(void *data, size_t length)
{
	uint32_t sect_len;
	uint8_t *section = data;

#define	MOVE(len) do {		\
	assert(length >= (len)); \
	section += (len);	\
	length -= (len);	\
} while (0)

	if (length == 0 || *section != 'A')
		return (NULL);
	MOVE(1);

	/* Read the section length */
	if (length < sizeof(sect_len))
		return (NULL);
	memcpy(&sect_len, section, sizeof(sect_len));

	/*
	 * The section length should be no longer than the section it is within
	 */
	if (sect_len > length)
		return (NULL);

	MOVE(sizeof(sect_len));

	/* Skip the vendor name */
	while (length != 0) {
		if (*section == '\0')
			break;
		MOVE(1);
	}
	if (length == 0)
		return (NULL);
	MOVE(1);

	while (length != 0) {
		uint32_t tag_length;

		switch(*section) {
		case 1: /* Tag_File */
			MOVE(1);
			if (length < sizeof(tag_length))
				return (NULL);
			memcpy(&tag_length, section, sizeof(tag_length));
			break;
		case 2: /* Tag_Section */
		case 3: /* Tag_Symbol */
		default:
			return (NULL);
		}
		/* At least space for the tag and size */
		if (tag_length <= 5)
			return (NULL);
		tag_length--;
		/* Check the tag fits */
		if (tag_length > length)
			return (NULL);

#define	MOVE_TAG(len) do {		\
	assert(tag_length >= (len));	\
	MOVE(len);			\
	tag_length -= (len);		\
} while(0)

		MOVE(sizeof(tag_length));
		tag_length -= sizeof(tag_length);

		while (tag_length != 0) {
			uint8_t tag;

			assert(tag_length >= length);

			tag = *section;
			MOVE_TAG(1);

			/*
			 * These tag values come from:
			 *
			 * Addenda to, and Errata in, the ABI for the
			 * ARM Architecture. Release 2.08, section 2.3.
			 */
			if (tag == 6) { /* == Tag_CPU_arch */
				uint8_t val;

				val = *section;
				/*
				 * We don't support values that require
				 * more than one byte.
				 */
				if (val & (1 << 7))
					return (NULL);

				/* We have an ARMv4 or ARMv5 */
				if (val <= 5)
					return ("arm");
				else if (val == 6) /* We have an ARMv6 */
					return ("armv6");
				else /* We have an ARMv7+ */
					return ("armv7");
			} else if (tag == 4 || tag == 5 || tag == 32 ||
			    tag == 65 || tag == 67) {
				while (*section != '\0' && length != 0)
					MOVE_TAG(1);
				if (tag_length == 0)
					return (NULL);
				/* Skip the last byte */
				MOVE_TAG(1);
			} else if ((tag >= 7 && tag <= 31) || tag == 34 ||
			    tag == 36 || tag == 38 || tag == 42 || tag == 44 ||
			    tag == 64 || tag == 66 || tag == 68 || tag == 70) {
				/* Skip the uleb128 data */
				while (*section & (1 << 7) && length != 0)
					MOVE_TAG(1);
				if (tag_length == 0)
					return (NULL);
				/* Skip the last byte */
				MOVE_TAG(1);
			} else
				return (NULL);
#undef MOVE_TAG
		}

		break;
	}
	return (NULL);
#undef MOVE
}

static const char *
elf_parse_arch(os_type_t ostype, Elf *elf, GElf_Ehdr *ehdr)
{
	switch (ehdr->e_machine) {
	case EM_386:
		return ("i386");
	case EM_X86_64:
		switch (ostype) {
		case OS_FREEBSD:
			return ("amd64");
		case OS_DRAGONFLY:
			return ("x86:64");
		default:
			return ("x86_64");
		}
	case EM_AARCH64:
		return ("aarch64");
	case EM_ARM:
		/* Only support EABI */
		if ((ehdr->e_flags & EF_ARM_EABIMASK) == 0) {
			return (NULL);
		}

		size_t shstrndx;
		elf_getshdrstrndx(elf, &shstrndx);

		GElf_Shdr shdr;
		Elf_Scn *scn = NULL;
		while ((scn = elf_nextscn(elf, scn)) != NULL) {
			if (gelf_getshdr(scn, &shdr) != &shdr) {
				break;
			}
			const char *sh_name = elf_strptr(elf, shstrndx, shdr.sh_name);
			if (sh_name == NULL) {
				continue;
			}
			if (STREQ(".ARM.attributes", sh_name)) {
				Elf_Data *data = elf_getdata(scn, NULL);
				return (aeabi_parse_arm_attributes(data->d_buf, data->d_size));
			}
		}
		break;
	case EM_PPC:
		return ("powerpc");
	case EM_PPC64:
		switch (ehdr->e_ident[EI_DATA]) {
		case ELFDATA2MSB:
			return ("powerpc64");
		case ELFDATA2LSB:
			return ("powerpc64le");
		}
		break;
	case EM_RISCV:
		switch (ehdr->e_ident[EI_CLASS]) {
		case ELFCLASS32:
			return ("riscv32");
		case ELFCLASS64:
			return ("riscv64");
		}
		break;
	}

	return (NULL);
}

static bool
elf_note_analyse(Elf_Data *data, GElf_Ehdr *elfhdr, struct os_info *oi)
{
	Elf_Note note;
	char *src;
	uint32_t gnu_abi_tag[4];
	char *note_os[6] = {"Linux", "GNU", "Solaris", "FreeBSD", "NetBSD", "Syllable"};
	int note_ost[6] = {OS_LINUX, OS_GNU, OS_SOLARIS, OS_FREEBSD, OS_NETBSD, OS_SYLLABLE};
	char *(*pnote_os)[6] = &note_os;
	char invalid_osname[] = "Unknown";
	uint32_t version = 0;
	int version_style = 1;

	src = data->d_buf;

	while ((uintptr_t)src < ((uintptr_t)data->d_buf + data->d_size)) {
		memcpy(&note, src, sizeof(Elf_Note));
		src += sizeof(Elf_Note);
		if ((strncmp ((const char *) src, "FreeBSD", note.n_namesz) == 0) ||
		    (strncmp ((const char *) src, "DragonFly", note.n_namesz) == 0) ||
		    (strncmp ((const char *) src, "NetBSD", note.n_namesz) == 0) ||
		    (note.n_namesz == 0)) {
			if (note.n_type == NT_VERSION) {
				version_style = 1;
				break;
			}
		}
		if (strncmp ((const char *) src, "GNU", note.n_namesz) == 0) {
			if (note.n_type == NT_GNU_ABI_TAG) {
				version_style = 2;
				break;
			}
		}
		src += roundup2(note.n_namesz + note.n_descsz, 4);
	}
	if ((uintptr_t)src >= ((uintptr_t)data->d_buf + data->d_size)) {
		return (false);
	}
	free(oi->name);
	if (version_style == 2) {
		/*
		 * NT_GNU_ABI_TAG
		 * Operating system (OS) ABI information.  The
		 * desc field contains 4 words:
		 * word 0: OS descriptor (ELF_NOTE_OS_LINUX, ELF_NOTE_OS_GNU, etc)
		 * word 1: major version of the ABI
		 * word 2: minor version of the ABI
		 * word 3: subminor version of the ABI
		 */
		src += roundup2(note.n_namesz, 4);
		if (elfhdr->e_ident[EI_DATA] == ELFDATA2MSB) {
			for (int wdndx = 0; wdndx < 4; wdndx++) {
				gnu_abi_tag[wdndx] = be32dec(src);
				src += 4;
			}
		} else {
			for (int wdndx = 0; wdndx < 4; wdndx++) {
				gnu_abi_tag[wdndx] = le32dec(src);
				src += 4;
			}
		}
		if (gnu_abi_tag[0] < 6) {
			oi->name = xstrdup((*pnote_os)[gnu_abi_tag[0]]);
			oi->ostype = note_ost[gnu_abi_tag[0]];
		} else {
			oi->name = xstrdup(invalid_osname);
			oi->ostype = OS_UNKNOWN;
		}
	} else {
		if (note.n_namesz == 0) {
			oi->name = xstrdup(invalid_osname);
			oi->ostype = OS_UNKNOWN;
		} else {
			oi->name = xstrdup(src);
			if (STREQ(src, "FreeBSD"))
				oi->ostype = OS_FREEBSD;
			else if (STREQ(src, "DragonFly"))
				oi->ostype = OS_DRAGONFLY;
			else if (STREQ(src, "NetBSD"))
				oi->ostype = OS_NETBSD;
		}
		src += roundup2(note.n_namesz, 4);
		if (elfhdr->e_ident[EI_DATA] == ELFDATA2MSB)
			version = be32dec(src);
		else
			version = le32dec(src);
	}

	free(oi->version);
	if (version_style == 2) {
		if (oi->ostype == OS_LINUX) {
			xasprintf(&oi->version, "%d.%d", gnu_abi_tag[1],
			    gnu_abi_tag[2]);
		} else {
			xasprintf(&oi->version, "%d.%d.%d", gnu_abi_tag[1],
			    gnu_abi_tag[2], gnu_abi_tag[3]);
		}
	} else {
		if (oi->osversion == 0) {
			oi->osversion = version;
			snprintf(oi->str_osversion, sizeof(oi->str_osversion), "%d", version);
		}
		if (oi->ostype == OS_DRAGONFLY) {
			xasprintf(&oi->version, "%d.%d", version / 100000, (((version / 100 % 1000)+1)/2)*2);
		} else if (oi->ostype == OS_NETBSD) {
			xasprintf(&oi->version, "%d", (version + 1000000) / 100000000);
		} else {
			xasprintf(&oi->version_major, "%d", version / 100000);
			xasprintf(&oi->version_minor, "%d", (version / 1000 % 100));
			xasprintf(&oi->version, "%d", version / 100000);
		}
	}

	return (true);
}

int
pkg_get_myarch_elfparse(int fd, struct os_info *oi)
{
	Elf *elf = NULL;
	GElf_Ehdr elfhdr;
	GElf_Shdr shdr;
	Elf_Data *data;
	Elf_Scn *scn = NULL;
	int ret = EPKG_OK;
	char *dest = oi->abi;
	size_t sz = sizeof(oi->abi);

	if (elf_version(EV_CURRENT) == EV_NONE) {
		pkg_emit_error("ELF library initialization failed: %s",
		    elf_errmsg(-1));
		return (EPKG_FATAL);
	}

	if ((elf = elf_begin(fd, ELF_C_READ, NULL)) == NULL) {
		ret = EPKG_FATAL;
		pkg_emit_error("elf_begin() failed: %s.", elf_errmsg(-1));
		goto cleanup;
	}

	if (gelf_getehdr(elf, &elfhdr) == NULL) {
		ret = EPKG_WARN;
		pkg_debug(1, "getehdr() failed: %s.", elf_errmsg(-1));
		goto cleanup;
	}

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		if (gelf_getshdr(scn, &shdr) != &shdr) {
			ret = EPKG_FATAL;
			pkg_emit_error("getshdr() failed: %s.", elf_errmsg(-1));
			goto cleanup;
		}

		if (shdr.sh_type == SHT_NOTE) {
			data = elf_getdata(scn, NULL);
			/*
			 * loop over all the note section and override what
			 * should be overridden if any
			 */
			elf_note_analyse(data, &elfhdr, oi);
		}
	}

	if (oi->name == NULL) {
		ret = EPKG_FATAL;
		pkg_emit_error("failed to get the note section");
		goto cleanup;
	}

	const char *arch = elf_parse_arch(oi->ostype, elf, &elfhdr);
	if (arch == NULL) {
		ret = EPKG_FATAL;
		pkg_emit_error("failed to determine the architecture");
		goto cleanup;
	}
	oi->arch = xstrdup(arch);

	snprintf(dest, sz, "%s:%s:%s", oi->name, oi->version, oi->arch);

cleanup:
	if (elf != NULL)
		elf_end(elf);
	return (ret);
}

int pkg_analyse_init_elf(const char* stage) {
	if (elf_version(EV_CURRENT) == EV_NONE)
		return (EPKG_FATAL);

	shlib_list_init();

	if (stage != NULL && pkg_object_bool(pkg_config_get("ALLOW_BASE_SHLIBS"))) {
		/* Do not check the return */
		shlib_list_from_stage(stage);
	}

	int ret = shlib_list_from_elf_hints(_PATH_ELF_HINTS);
	return ret;
}

int pkg_analyse_elf(const bool developer_mode, struct pkg *pkg, const char *fpath) {
		int ret = analyse_elf(pkg, fpath);
		if (developer_mode) {
			if (ret != EPKG_OK && ret != EPKG_END) {
				return EPKG_WARN;
			}
			analyse_fpath(pkg, fpath);
		}
		return ret;
}

int pkg_analyse_close_elf() {
	shlib_list_free();
	return EPKG_OK;
}
