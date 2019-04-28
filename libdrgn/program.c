// Copyright 2018-2019 - Omar Sandoval
// SPDX-License-Identifier: GPL-3.0+

#include <byteswap.h>
#include <fcntl.h>
#include <fts.h>
#include <gelf.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/vfs.h>

#include "internal.h"
#include "dwarf_index.h"
#include "language.h"
#include "memory_reader.h"
#include "symbol_index.h"
#include "program.h"
#include "read.h"
#include "type_index.h"

/* This definition was added to elf.h in glibc 2.18. */
#ifndef NT_FILE
#define NT_FILE 0x46494c45
#endif

LIBDRGN_PUBLIC enum drgn_program_flags
drgn_program_flags(struct drgn_program *prog)
{
	return prog->flags;
}

LIBDRGN_PUBLIC uint8_t drgn_program_word_size(struct drgn_program *prog)
{
	return prog->tindex->word_size;
}

LIBDRGN_PUBLIC bool drgn_program_is_little_endian(struct drgn_program *prog)
{
	return prog->tindex->little_endian;
}

void drgn_program_init(struct drgn_program *prog,
		       struct drgn_memory_reader *reader,
		       struct drgn_type_index *tindex,
		       struct drgn_symbol_index *sindex)
{
	prog->reader = reader;
	prog->tindex = tindex;
	prog->sindex = sindex;
	prog->cleanup = NULL;
	prog->flags = 0;
}

void drgn_program_deinit(struct drgn_program *prog)
{
	struct drgn_cleanup *cleanup;

	drgn_symbol_index_destroy(prog->sindex);
	drgn_type_index_destroy(prog->tindex);
	drgn_memory_reader_destroy(prog->reader);

	cleanup = prog->cleanup;
	while (cleanup) {
		struct drgn_cleanup *next;

		next = cleanup->next;
		cleanup->cb(cleanup->arg);
		free(cleanup);
		cleanup = next;
	}
}

struct drgn_error *drgn_program_add_cleanup(struct drgn_program *prog,
					    void (*cb)(void *), void *arg)
{
	struct drgn_cleanup *cleanup;

	cleanup = malloc(sizeof(*cleanup));
	if (!cleanup)
		return &drgn_enomem;
	cleanup->cb = cb;
	cleanup->arg = arg;
	cleanup->next = prog->cleanup;
	prog->cleanup = cleanup;
	return NULL;
}

bool drgn_program_remove_cleanup(struct drgn_program *prog, void (*cb)(void *),
				 void *arg)
{
	struct drgn_cleanup **cleanupp = &prog->cleanup;

	while (*cleanupp) {
		struct drgn_cleanup *cleanup = *cleanupp;

		if (cleanup->cb == cb && cleanup->arg == arg) {
			*cleanupp = cleanup->next;
			free(cleanup);
			return true;
		}
		cleanupp = &cleanup->next;
	}
	return false;
}

static struct drgn_error *get_module_name(Elf_Scn *modinfo_scn,
					  const char **ret)
{
	struct drgn_error *err;
	Elf_Data *data;
	const char *p, *end;

	err = read_elf_section(modinfo_scn, &data);
	if (err)
		return err;

	p = data->d_buf;
	end = p + data->d_size;
	while (p < end) {
		const char *nul;

		nul = memchr(p, 0, end - p);
		if (!nul)
			break;
		if (strncmp(p, "name=", 5) == 0) {
			*ret = p + 5;
			return NULL;
		}
		p = nul + 1;
	}
	return drgn_error_create(DRGN_ERROR_LOOKUP,
				 "could not find name in .modinfo section");
}

static struct drgn_error *find_elf_symbol(Elf *elf, Elf_Scn *symtab_scn,
					  const char *name, uint64_t address,
					  bool by_address, GElf_Sym *sym,
					  Elf32_Word *shndx)
{
	struct drgn_error *err;
	int xndxscnidx;
	GElf_Shdr shdr_mem, *shdr;
	Elf_Data *xndx_data = NULL, *data;
	size_t num_syms, i;

	xndxscnidx = elf_scnshndx(symtab_scn);
	if (xndxscnidx > 0)
		xndx_data = elf_getdata(elf_getscn(elf, xndxscnidx), NULL);

	err = read_elf_section(symtab_scn, &data);
	if (err)
		return err;
	shdr = gelf_getshdr(symtab_scn, &shdr_mem);
	if (!shdr)
		return drgn_error_libelf();

	num_syms = data->d_size / (gelf_getclass(elf) == ELFCLASS32 ?
				   sizeof(Elf32_Sym) : sizeof(Elf64_Sym));
	for (i = 0; i < num_syms; i++) {
		const char *sym_name;

		if (!gelf_getsymshndx(data, xndx_data, i, sym, shndx))
			continue;
		if (by_address) {
			if (sym->st_value == address)
				return NULL;
		} else {
			sym_name = elf_strptr(elf, shdr->sh_link, sym->st_name);
			if (sym_name && strcmp(sym_name, name) == 0)
				return NULL;
		}
	}
	return drgn_error_format(DRGN_ERROR_LOOKUP,
				 "could not find %s symbol", name);
}

static struct drgn_error *get_symbol_section_name(Elf *elf, size_t shstrndx,
						  Elf_Scn *symtab_scn,
						  const char *name,
						  uint64_t address,
						  const char **ret)
{
	struct drgn_error *err;
	GElf_Sym sym;
	Elf32_Word shndx;
	Elf_Scn *scn;
	GElf_Shdr shdr_mem, *shdr;
	const char *scnname;

	err = find_elf_symbol(elf, symtab_scn, name, address, true, &sym,
			      &shndx);
	if (err)
		return err;

	if (sym.st_shndx != SHN_XINDEX)
		shndx = sym.st_shndx;
	scn = elf_getscn(elf, shndx);
	if (!scn)
		return drgn_error_libelf();
	shdr = gelf_getshdr(scn, &shdr_mem);
	if (!shdr)
		return drgn_error_libelf();
	scnname = elf_strptr(elf, shstrndx, shdr->sh_name);
	if (!scnname)
		return drgn_error_libelf();
	*ret = scnname;
	return NULL;
}

static struct drgn_error *find_module(struct drgn_object *mod,
				      const char *module_name)
{
	struct drgn_error *err;
	struct drgn_qualified_type module_type;
	struct drgn_object node, mod_name;
	uint64_t head;

	err = drgn_program_find_type(mod->prog, "struct module", NULL,
				     &module_type);
	if (err)
		return err;

	drgn_object_init(&node, mod->prog);
	drgn_object_init(&mod_name, mod->prog);

	err = drgn_program_find_object(mod->prog, "modules", NULL,
				       DRGN_FIND_OBJECT_VARIABLE, &node);
	if (err)
		goto out;
	err = drgn_object_address_of(&node, &node);
	if (err)
		goto out;
	err = drgn_object_read(&node, &node);
	if (err)
		goto out;
	err = drgn_object_read_unsigned(&node, &head);
	if (err)
		goto out;

	for (;;) {
		uint64_t addr;
		char *name;

		err = drgn_object_member_dereference(&node, &node, "next");
		if (err)
			goto out;
		err = drgn_object_read(&node, &node);
		if (err)
			goto out;
		err = drgn_object_read_unsigned(&node, &addr);
		if (err)
			goto out;
		if (addr == head) {
			err = drgn_error_format(DRGN_ERROR_LOOKUP,
						"%s is not loaded",
						module_name);
			goto out;
		}

		err = drgn_object_container_of(mod, &node, module_type, "list");
		if (err)
			goto out;
		err = drgn_object_member_dereference(&mod_name, mod, "name");
		if (err)
			goto out;

		err = drgn_object_read_c_string(&mod_name, &name);
		if (err)
			goto out;
		if (strcmp(name, module_name) == 0) {
			free(name);
			break;
		}
		free(name);
	}

	err = NULL;
out:
	drgn_object_deinit(&mod_name);
	drgn_object_deinit(&node);
	return err;
}

static struct drgn_error *find_section_address(struct drgn_object *mod,
					       const char *section_name,
					       uint64_t *ret)
{
	struct drgn_error *err;
	struct drgn_object attrs, attr, tmp;
	uint64_t i, nsections;

	drgn_object_init(&attrs, mod->prog);
	drgn_object_init(&attr, mod->prog);
	drgn_object_init(&tmp, mod->prog);

	err = drgn_object_member_dereference(&attrs, mod, "sect_attrs");
	if (err)
		goto out;
	err = drgn_object_member_dereference(&tmp, &attrs, "nsections");
	if (err)
		goto out;
	err = drgn_object_read_unsigned(&tmp, &nsections);
	if (err)
		goto out;
	err = drgn_object_member_dereference(&attrs, &attrs, "attrs");
	if (err)
		goto out;

	for (i = 0; i < nsections; i++) {
		char *name;

		err = drgn_object_subscript(&attr, &attrs, i);
		if (err)
			goto out;
		err = drgn_object_member(&tmp, &attr, "name");
		if (err)
			goto out;

		err = drgn_object_read_c_string(&tmp, &name);
		if (err)
			goto out;
		if (strcmp(name, section_name) == 0) {
			free(name);
			err = drgn_object_member(&tmp, &attr, "address");
			if (err)
				goto out;
			err = drgn_object_read_unsigned(&tmp, ret);
			goto out;
		}
		free(name);
	}

	err = drgn_error_format(DRGN_ERROR_LOOKUP,
				"could not find module section %s",
				section_name);
out:
	drgn_object_deinit(&tmp);
	drgn_object_deinit(&attr);
	drgn_object_deinit(&attrs);
	return err;
}

static struct drgn_error *
kernel_relocation_hook(struct drgn_program *prog, const char *name,
		       Dwarf_Die *die, struct drgn_symbol *sym)
{
	struct drgn_error *err;
	Elf *elf;
	GElf_Ehdr ehdr_mem, *ehdr;
	size_t shstrndx;
	Elf_Scn *scn, *modinfo_scn = NULL, *symtab_scn = NULL;
	const char *section_name, *module_name;
	struct drgn_object mod;
	uint64_t section_address;

	elf = dwarf_getelf(dwarf_cu_getdwarf(die->cu));
	ehdr = gelf_getehdr(elf, &ehdr_mem);
	if (!ehdr)
		return drgn_error_libelf();

	/* vmlinux is executable, kernel modules are relocatable. */
	if (ehdr->e_type == ET_EXEC) {
		sym->address += prog->vmcoreinfo.kaslr_offset;
		return NULL;
	}

	if (elf_getshdrstrndx(elf, &shstrndx))
		return drgn_error_libelf();

	/* Find .modinfo and .symtab. */
	scn = NULL;
	while ((scn = elf_nextscn(elf, scn))) {
		GElf_Shdr *shdr, shdr_mem;
		const char *scnname;

		shdr = gelf_getshdr(scn, &shdr_mem);
		if (!shdr)
			continue;

		scnname = elf_strptr(elf, shstrndx, shdr->sh_name);
		if (!scnname)
			continue;
		if (strcmp(scnname, ".modinfo") == 0)
			modinfo_scn = scn;
		else if (strcmp(scnname, ".symtab") == 0)
			symtab_scn = scn;
	}
	if (!modinfo_scn) {
		return drgn_error_create(DRGN_ERROR_LOOKUP,
					 "could not find .modinfo section");
	}
	if (!symtab_scn) {
		return drgn_error_create(DRGN_ERROR_LOOKUP,
					 "could not find .symtab section");
	}

	/* Find the name of the module in .modinfo. */
	err = get_module_name(modinfo_scn, &module_name);
	if (err)
		return err;

	/* Find the name of the section containing the symbol. */
	err = get_symbol_section_name(elf, shstrndx, symtab_scn, name,
				      sym->address, &section_name);
	if (err)
		return err;

	drgn_object_init(&mod, prog);

	/* Find the (struct module *) from its name. */
	err = find_module(&mod, module_name);
	if (err) {
		drgn_object_deinit(&mod);
		return err;
	}

	/* Find the section's base address from its name. */
	err = find_section_address(&mod, section_name, &section_address);
	drgn_object_deinit(&mod);
	if (err)
		return err;

	sym->address += section_address;
	return NULL;
}

static struct drgn_error *
userspace_relocation_hook(struct drgn_program *prog, const char *name,
			  Dwarf_Die *die, struct drgn_symbol *sym)
{
	Elf *elf;
	size_t phnum, i;
	uint64_t file_offset;

	elf = dwarf_getelf(dwarf_cu_getdwarf(die->cu));
	if (elf_getphdrnum(elf, &phnum) != 0)
		return drgn_error_libelf();

	for (i = 0; i < phnum; i++) {
		GElf_Phdr phdr_mem, *phdr;

		phdr = gelf_getphdr(elf, i, &phdr_mem);
		if (!phdr)
			return drgn_error_libelf();

		if (phdr->p_type == PT_LOAD &&
		    phdr->p_vaddr <= sym->address &&
		    sym->address < phdr->p_vaddr + phdr->p_memsz) {
			file_offset = (phdr->p_offset + sym->address -
				       phdr->p_vaddr);
			break;
		}
	}
	if (i >= phnum) {
		return drgn_error_format(DRGN_ERROR_LOOKUP,
					 "could not find segment containing %s",
					 name);
	}

	for (i = 0; i < prog->num_mappings; i++) {
		struct file_mapping *mapping = &prog->mappings[i];
		uint64_t mapping_size;

		mapping_size = mapping->end - mapping->start;
		if (mapping->elf == elf &&
		    mapping->file_offset <= file_offset &&
		    file_offset < mapping->file_offset + mapping_size) {
			sym->address = (mapping->start + file_offset -
					mapping->file_offset);
			return NULL;
		}
	}
	return drgn_error_format(DRGN_ERROR_LOOKUP,
				 "could not find file mapping containing %s",
				 name);
}

/*
 * Returns NULL if a mapping was appended, &drgn_stop if the mapping was merged,
 * non-NULL on error.
 */
static struct drgn_error *append_file_mapping(uint64_t start, uint64_t end,
					      uint64_t file_offset, char *path,
					      struct file_mapping **mappings,
					      size_t *num_mappings,
					      size_t *capacity)
{
	struct file_mapping *mapping;

	if (start > end) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "file memory mapping has negative length");
	} else if (start == end) {
		return NULL;
	}

	/*
	 * There may be separate mappings for adjacent areas of a file (e.g., if
	 * the mappings have different permissions). Make sure to merge those.
	 */
	if (*num_mappings) {
		uint64_t length;

		mapping = &(*mappings)[*num_mappings - 1];
		length = mapping->end - mapping->start;
		if (mapping->end == start &&
		    mapping->file_offset + length == file_offset &&
		    strcmp(mapping->path, path) == 0) {
			mapping->end = end;
			return &drgn_stop;
		}
	}

	if (*num_mappings >= *capacity) {
		size_t new_capacity;

		if (*capacity == 0)
			new_capacity = 1;
		else
			new_capacity = *capacity * 2;
		if (!resize_array(mappings, new_capacity))
			return &drgn_enomem;
		*capacity = new_capacity;
	}

	mapping = &(*mappings)[(*num_mappings)++];
	mapping->start = start;
	mapping->end = end;
	mapping->file_offset = file_offset;
	mapping->path = path;
	return NULL;
}

static struct drgn_error *parse_nt_file(const char *desc, size_t descsz,
					bool is_64_bit,
					struct file_mapping **mappings,
					size_t *num_mappings,
					size_t *mappings_capacity)
{
	struct drgn_error *err;
	uint64_t count, page_size, i;
	const char *p = desc, *q, *end = &desc[descsz];
	size_t paths_offset;
	bool bswap = false;

	if (is_64_bit) {
		if (!read_u64(&p, end, bswap, &count) ||
		    !read_u64(&p, end, bswap, &page_size) ||
		    __builtin_mul_overflow(count, 24U, &paths_offset))
			goto invalid;
	} else {
		if (!read_u32_into_u64(&p, end, bswap, &count) ||
		    !read_u32_into_u64(&p, end, bswap, &page_size) ||
		    __builtin_mul_overflow(count, 12U, &paths_offset))
			goto invalid;
	}

	if (!read_in_bounds(p, end, paths_offset))
		goto invalid;
	q = p + paths_offset;
	for (i = 0; i < count; i++) {
		uint64_t mapping_start, mapping_end, file_offset;
		const char *path;
		size_t len;

		/* We already did the bounds check above. */
		if (is_64_bit) {
			read_u64_nocheck(&p, bswap, &mapping_start);
			read_u64_nocheck(&p, bswap, &mapping_end);
			read_u64_nocheck(&p, bswap, &file_offset);
		} else {
			read_u32_into_u64_nocheck(&p, bswap, &mapping_start);
			read_u32_into_u64_nocheck(&p, bswap, &mapping_end);
			read_u32_into_u64_nocheck(&p, bswap, &file_offset);
		}
		file_offset *= page_size;

		if (!read_string(&q, end, &path, &len))
			goto invalid;
		err = append_file_mapping(mapping_start, mapping_end, file_offset,
					  (char *)path, mappings, num_mappings,
					  mappings_capacity);
		if (!err) {
			struct file_mapping *mapping;

			/*
			 * The mapping wasn't merged, so actually allocate the
			 * path now.
			 */
			mapping = &(*mappings)[*num_mappings - 1];
			mapping->path = malloc(len + 1);
			if (!mapping->path)
				return &drgn_enomem;
			memcpy(mapping->path, path, len + 1);
		} else if (err->code != DRGN_ERROR_STOP) {
			return err;
		}
	}

	return NULL;

invalid:
	return drgn_error_create(DRGN_ERROR_ELF_FORMAT, "invalid NT_FILE note");
}

static inline bool linematch(const char **line, const char *prefix)
{
	size_t len = strlen(prefix);

	if (strncmp(*line, prefix, len) == 0) {
		*line += len;
		return true;
	} else {
		return false;
	}
}

static struct drgn_error *parse_vmcoreinfo(const char *desc, size_t descsz,
					   struct vmcoreinfo *ret)
{
	const char *line = desc, *end = &desc[descsz];

	ret->osrelease[0] = '\0';
	ret->kaslr_offset = 0;
	while (line < end) {
		const char *newline;

		newline = memchr(line, '\n', end - line);
		if (!newline)
			break;

		if (linematch(&line, "OSRELEASE=")) {
			if ((size_t)(newline - line) >=
			    sizeof(ret->osrelease)) {
				return drgn_error_create(DRGN_ERROR_OTHER,
							 "OSRELEASE in VMCOREINFO is too long");
			}
			memcpy(ret->osrelease, line, newline - line);
			ret->osrelease[newline - line] = '\0';
		} else if (linematch(&line, "KERNELOFFSET=")) {
			unsigned long long kerneloffset;
			char *nend;

			errno = 0;
			kerneloffset = strtoull(line, &nend, 16);
			if (errno == ERANGE) {
				return drgn_error_create(DRGN_ERROR_OVERFLOW,
							 "KERNELOFFSET in VMCOREINFO is too large");
			} else if (errno || nend == line || nend != newline) {
				return drgn_error_create(DRGN_ERROR_OVERFLOW,
							 "KERNELOFFSET in VMCOREINFO is invalid");
			}
			ret->kaslr_offset = kerneloffset;
		}
		line = newline + 1;
	}
	if (!ret->osrelease[0]) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "VMCOREINFO does not contain valid OSRELEASE");
	}
	return NULL;
}

static struct drgn_error *
read_vmcoreinfo_from_sysfs(struct drgn_memory_reader *reader,
			   struct vmcoreinfo *ret)
{
	struct drgn_error *err;
	FILE *file;
	uint64_t address, size;
	char *buf;
	Elf64_Nhdr *nhdr;

	file = fopen("/sys/kernel/vmcoreinfo", "r");
	if (!file) {
		return drgn_error_create_os(errno, "/sys/kernel/vmcoreinfo",
					    "fopen");
	}
	if (fscanf(file, "%" SCNx64 " %" SCNx64, &address, &size) != 2) {
		fclose(file);
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "could not parse /sys/kernel/vmcoreinfo");
	}
	fclose(file);

	buf = malloc64(size);
	if (!buf)
		return &drgn_enomem;

	err = drgn_memory_reader_read(reader, buf, address, size, true);
	if (err)
		goto out;

	/*
	 * The first 12 bytes are the Elf{32,64}_Nhdr (it's the same in both
	 * formats). The name is padded up to 4 bytes, so the descriptor starts
	 * at byte 24.
	 */
	nhdr = (Elf64_Nhdr *)buf;
	if (size < 24 || nhdr->n_namesz != 11 ||
	    memcmp(buf + sizeof(*nhdr), "VMCOREINFO", 10) != 0 ||
	    nhdr->n_descsz > size - 24) {
		err = drgn_error_create(DRGN_ERROR_OTHER,
					"VMCOREINFO in /sys/kernel/vmcoreinfo is invalid");
		goto out;
	}

	err = parse_vmcoreinfo(buf + 24, nhdr->n_descsz, ret);
out:
	free(buf);
	return err;
}

static struct drgn_error *proc_kallsyms_symbol_addr(const char *name,
						    uint64_t *ret)
{
	struct drgn_error *err;
	FILE *file;
	char *line = NULL;
	size_t n = 0;
	bool found = false;

	file = fopen("/proc/kallsyms", "r");
	if (!file)
		return drgn_error_create_os(errno, "/proc/kallsyms", "fopen");

	while (errno = 0, getline(&line, &n, file) != -1) {
		char *addr_str, *sym_str, *saveptr;
		unsigned long long addr;
		char *end;

		addr_str = strtok_r(line, "\t ", &saveptr);
		if (!addr_str || !*addr_str)
			goto invalid;
		if (!strtok_r(NULL, "\t ", &saveptr))
			goto invalid;
		sym_str = strtok_r(NULL, "\t\n ", &saveptr);
		if (!sym_str)
			goto invalid;

		if (strcmp(sym_str, name) != 0)
			continue;

		errno = 0;
		addr = strtoull(line, &end, 16);
		if ((addr == ULLONG_MAX && errno == ERANGE) || *end)
			goto invalid;
		*ret = addr;
		found = true;
		break;
	}
	if (errno) {
		err = drgn_error_create_os(errno, "/proc/kallsyms", "getline");
	} else if (!found) {
		err = drgn_error_format(DRGN_ERROR_OTHER,
					"could not find %s symbol in /proc/kallsyms",
					name);
	} else {
		err = NULL;
	}
	free(line);
	fclose(file);
	return err;

invalid:
	return drgn_error_create(DRGN_ERROR_OTHER,
				 "could not parse /proc/kallsyms");
}

static const char * const vmlinux_paths[] = {
	"/usr/lib/debug/lib/modules/%s/vmlinux",
	"/boot/vmlinux-%s",
	"/lib/modules/%s/build/vmlinux",
};

static struct drgn_error *vmlinux_symbol_addr(const char *osrelease,
					      const char *name, uint64_t *ret)
{
	struct drgn_error *err;
	size_t i;
	bool found_vmlinux = false;

	for (i = 0; i < ARRAY_SIZE(vmlinux_paths); i++) {
		char buf[256];
		int fd;
		Elf *elf;
		size_t shstrndx;
		Elf_Scn *scn;
		GElf_Sym sym;

		snprintf(buf, sizeof(buf), vmlinux_paths[i], osrelease);

		fd = open(buf, O_RDONLY);
		if (fd == -1)
			continue;

		found_vmlinux = true;

		elf = elf_begin(fd, ELF_C_READ, NULL);
		if (!elf) {
			close(fd);
			return drgn_error_libelf();
		}

		if (elf_getshdrstrndx(elf, &shstrndx)) {
			err = drgn_error_libelf();
			goto err;
		}

		scn = NULL;
		while ((scn = elf_nextscn(elf, scn))) {
			GElf_Shdr *shdr, shdr_mem;
			const char *scnname;

			shdr = gelf_getshdr(scn, &shdr_mem);
			if (!shdr)
				continue;

			scnname = elf_strptr(elf, shstrndx, shdr->sh_name);
			if (!scnname)
				continue;
			if (strcmp(scnname, ".symtab") == 0)
				break;
		}
		if (!scn) {
			elf_end(elf);
			close(fd);
			continue;
		}

		err = find_elf_symbol(elf, scn, name, 0, false, &sym, NULL);
		if (!err)
			*ret = sym.st_value;
err:
		elf_end(elf);
		close(fd);
		return err;
	}
	if (found_vmlinux) {
		return drgn_error_create(DRGN_ERROR_MISSING_DEBUG,
					 "vmlinux does not have symbol table");
	} else {
		return drgn_error_create(DRGN_ERROR_MISSING_DEBUG,
					 "could not find vmlinux");
	}
}

static struct drgn_error *get_fallback_vmcoreinfo(struct vmcoreinfo *ret)
{
	struct drgn_error *err;
	struct utsname uts;
	size_t release_len;
	uint64_t kallsyms_addr, elf_addr;

	if (uname(&uts) == -1)
		return drgn_error_create_os(errno, NULL, "uname");

	release_len = strlen(uts.release);
	if (release_len >= sizeof(ret->osrelease)) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "uname release is too long");
	}
	memcpy(ret->osrelease, uts.release, release_len + 1);

	err = proc_kallsyms_symbol_addr("_stext", &kallsyms_addr);
	if (err)
		return err;

	err = vmlinux_symbol_addr(uts.release, "_stext", &elf_addr);
	if (err)
		return err;

	ret->kaslr_offset = kallsyms_addr - elf_addr;
	return NULL;
}

static bool fts_name_endswith(FTSENT *ent, const char *suffix)
{
	size_t len = strlen(suffix);

	return (ent->fts_namelen >= len &&
		memcmp(&ent->fts_name[ent->fts_namelen - len], suffix,
		       len) == 0);
}

static struct drgn_error *open_kernel_files(struct drgn_dwarf_index *dindex,
					    const char *osrelease, bool verbose)
{
	static const char * const module_paths[] = {
		"/usr/lib/debug/lib/modules/%s/kernel",
		"/lib/modules/%s/kernel",
	};
	/* module_extensions[i] corresponds to module_paths[i]. */
	static const char * const module_extensions[] = {
		".ko.debug",
		".ko",
	};
	_Static_assert(ARRAY_SIZE(module_paths) ==
		       ARRAY_SIZE(module_extensions),
		       "mismatched number of module paths and extensions");
	char buf[256];
	struct drgn_error *err;
	bool found_vmlinux = false, found_modules = false;
	size_t i;
	const size_t max_no_symbols = 5;
	size_t no_symbols = 0;

	for (i = 0; i < ARRAY_SIZE(vmlinux_paths); i++) {
		snprintf(buf, sizeof(buf), vmlinux_paths[i], osrelease);
		err = drgn_dwarf_index_open(dindex, buf, NULL);
		if (err) {
			if (err->code == DRGN_ERROR_OS &&
			    err->errnum == ENOENT) {
				drgn_error_destroy(err);
				continue;
			}
			if (err->code == DRGN_ERROR_MISSING_DEBUG) {
				found_vmlinux = true;
				drgn_error_destroy(err);
				continue;
			}
			return err;
		}
		break;
	}
	if (i >= ARRAY_SIZE(vmlinux_paths)) {
		if (found_vmlinux) {
			return drgn_error_create(DRGN_ERROR_MISSING_DEBUG,
						 "vmlinux does not have debug information");
		} else {
			return drgn_error_create(DRGN_ERROR_MISSING_DEBUG,
						 "could not find vmlinux");
		}

	}

	for (i = 0; i < ARRAY_SIZE(module_paths) && !found_modules; i++) {
		char * const paths[] = {buf, NULL};
		FTS *fts;
		FTSENT *ent;

		snprintf(buf, sizeof(buf), module_paths[i], osrelease);
		fts = fts_open(paths, FTS_LOGICAL | FTS_NOCHDIR | FTS_NOSTAT,
			       NULL);
		if (!fts) {
			if (errno == ENOENT)
				continue;
			return drgn_error_create_os(errno, buf, "fts_open");
		}

		err = NULL;
		while ((ent = fts_read(fts))) {
			if ((ent->fts_info != FTS_F &&
			     ent->fts_info != FTS_NSOK) ||
			    !fts_name_endswith(ent, module_extensions[i]))
				continue;
			if (ent->fts_info == FTS_NSOK) {
				struct stat st;

				if (stat(ent->fts_accpath, &st) == -1 ||
				    (st.st_mode & S_IFMT) != S_IFREG)
					continue;
			}
			found_modules = true;
			err = drgn_dwarf_index_open(dindex, ent->fts_accpath,
						    NULL);
			if (err && err->code == DRGN_ERROR_MISSING_DEBUG) {
				if (verbose) {
					if (no_symbols == 0) {
						fprintf(stderr,
							"missing debug information for modules:\n");
					}
					if (no_symbols < max_no_symbols) {
						int len;

						len = (ent->fts_namelen -
						       strlen(module_extensions[i]));
						fprintf(stderr, "%.*s\n", len,
							ent->fts_name);
					}
					no_symbols++;
				}
				drgn_error_destroy(err);
				err = NULL;
				continue;
			} else if (err) {
				break;
			}
		}
		if (!err && errno)
			err = drgn_error_create_os(errno, buf, "fts_read");

		fts_close(fts);
		if (err)
			return err;
	}
	if (verbose) {
		if (!found_modules)
			fprintf(stderr, "could not find kernel modules\n");
		if (no_symbols > max_no_symbols) {
			fprintf(stderr, "... %zu more\n",
				no_symbols - max_no_symbols);
		}
	}

	return NULL;
}

static struct drgn_error *open_userspace_files(struct drgn_dwarf_index *dindex,
					       struct file_mapping *mappings,
					       size_t num_mappings)
{
	struct drgn_error *err;
	size_t i;
	bool success = false;

	for (i = 0; i < num_mappings; i++) {
		err = drgn_dwarf_index_open(dindex, mappings[i].path,
					    &mappings[i].elf);
		if (err) {
			mappings[i].elf = NULL;
			if ((err->code == DRGN_ERROR_OS &&
			     err->errnum == ENOENT) ||
			    err == &drgn_not_elf ||
			    err->code == DRGN_ERROR_MISSING_DEBUG) {
				drgn_error_destroy(err);
				continue;
			}
			return err;
		}
		success = true;
	}
	if (!success) {
		return drgn_error_create(DRGN_ERROR_MISSING_DEBUG,
					 "no debug information found");
	}
	return NULL;
}

static void free_file_mappings(struct file_mapping *mappings,
			       size_t num_mappings)
{
	size_t i;

	for (i = 0; i < num_mappings; i++)
		free(mappings[i].path);
	free(mappings);
}

static void cleanup_fd(void *arg)
{
	close((intptr_t)arg);
}

static void cleanup_dwarf_index(void *arg)
{
	drgn_dwarf_index_destroy(arg);
}

static void cleanup_dwarf_type_cache(void *arg)
{
	drgn_dwarf_type_cache_destroy(arg);
}

static void cleanup_file_mappings(void *arg)
{
	struct drgn_program *prog = arg;

	free_file_mappings(prog->mappings, prog->num_mappings);
}

#define PROGRAM_DWARF_INDEX_FLAGS (DRGN_DWARF_INDEX_TYPES |		\
				   DRGN_DWARF_INDEX_VARIABLES |		\
				   DRGN_DWARF_INDEX_ENUMERATORS |	\
				   DRGN_DWARF_INDEX_FUNCTIONS)

static Elf_Type note_header_type(GElf_Phdr *phdr)
{
#if _ELFUTILS_PREREQ(0, 175)
	if (phdr->p_align == 8)
		return ELF_T_NHDR8;
#endif
	return ELF_T_NHDR;
}

struct drgn_error *drgn_program_init_core_dump(struct drgn_program *prog,
					       const char *path, bool verbose)
{
	struct drgn_error *err;
	int fd;
	Elf *elf;
	GElf_Ehdr ehdr_mem, *ehdr;
	size_t phnum, i, num_file_segments;
	struct drgn_memory_file_segment *file_segments = NULL;
	struct drgn_memory_file_segment *current_file_segment;
	struct file_mapping *mappings = NULL;
	size_t num_mappings = 0, mappings_capacity = 0;
	struct vmcoreinfo vmcoreinfo;
	bool is_64_bit, have_nt_file = false;
	bool have_nt_taskstruct = false, have_vmcoreinfo = false;
	bool have_non_zero_phys_addr = false, is_proc_kcore;
	struct drgn_memory_reader *reader;
	struct drgn_dwarf_index *dindex;
	struct drgn_type_index *tindex;
	struct drgn_dwarf_type_cache *dtcache;
	struct drgn_dwarf_symbol_index *dsindex;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return drgn_error_create_os(errno, path, "open");

	elf_version(EV_CURRENT);

	elf = elf_begin(fd, ELF_C_READ, NULL);
	if (!elf) {
		err = drgn_error_libelf();
		goto out_fd;
	}

	ehdr = gelf_getehdr(elf, &ehdr_mem);
	if (!ehdr) {
		err = drgn_error_libelf();
		goto out_elf;
	}

	if (ehdr->e_type != ET_CORE) {
		err = drgn_error_format(DRGN_ERROR_INVALID_ARGUMENT,
					"not an ELF core file");
		goto out_elf;
	}

	is_64_bit = ehdr->e_ident[EI_CLASS] == ELFCLASS64;

	if (elf_getphdrnum(elf, &phnum) != 0) {
		err = drgn_error_libelf();
		goto out_elf;
	}

	/*
	 * First pass: count the number of loadable segments and check if p_addr
	 * is valid.
	 */
	num_file_segments = 0;
	for (i = 0; i < phnum; i++) {
		GElf_Phdr phdr_mem, *phdr;

		phdr = gelf_getphdr(elf, i, &phdr_mem);
		if (!phdr) {
			err = drgn_error_libelf();
			goto out_elf;
		}

		if (phdr->p_type == PT_LOAD) {
			if (phdr->p_paddr)
				have_non_zero_phys_addr = true;
			num_file_segments++;
		}
	}

	err = drgn_memory_reader_create(&reader);
	if (err)
		goto out_elf;

	file_segments = malloc_array(num_file_segments, sizeof(*file_segments));
	if (!file_segments)
		goto out_reader;
	current_file_segment = file_segments;

	/* Second pass: add the segments and parse notes. */
	for (i = 0; i < phnum; i++) {
		GElf_Phdr phdr_mem, *phdr;

		phdr = gelf_getphdr(elf, i, &phdr_mem);
		if (!phdr) {
			err = drgn_error_libelf();
			goto out_mappings;
		}

		if (phdr->p_type == PT_LOAD) {
			uint64_t phys_addr;

			/*
			 * If this happens, then the number of segments changed
			 * since the first pass. That's probably impossible, but
			 * skip it just in case.
			 */
			if (current_file_segment ==
			    file_segments + num_file_segments)
				continue;
			current_file_segment->file_offset = phdr->p_offset;
			current_file_segment->file_size = phdr->p_filesz;
			current_file_segment->fd = fd;
			phys_addr = (have_non_zero_phys_addr ? phdr->p_paddr :
				     UINT64_MAX);
			err = drgn_memory_reader_add_segment(reader,
							     phdr->p_vaddr,
							     phys_addr,
							     phdr->p_memsz,
							     drgn_read_memory_file,
							     current_file_segment);
			if (err)
				goto out_mappings;
			current_file_segment++;
		} else if (phdr->p_type == PT_NOTE) {
			Elf_Data *data;
			size_t offset;
			GElf_Nhdr nhdr;
			size_t name_offset, desc_offset;

			data = elf_getdata_rawchunk(elf, phdr->p_offset,
						    phdr->p_filesz,
						    note_header_type(phdr));
			if (!data) {
				err = drgn_error_libelf();
				goto out_mappings;
			}

			offset = 0;
			while (offset < data->d_size &&
			       (offset = gelf_getnote(data, offset, &nhdr,
						      &name_offset,
						      &desc_offset))) {
				const char *name, *desc;

				name = (char *)data->d_buf + name_offset;
				desc = (char *)data->d_buf + desc_offset;
				if (strncmp(name, "CORE", nhdr.n_namesz) == 0) {
					if (nhdr.n_type == NT_FILE) {
						err = parse_nt_file(desc,
								    nhdr.n_descsz,
								    is_64_bit,
								    &prog->mappings,
								    &prog->num_mappings,
								    &mappings_capacity);
						if (err)
							goto out_mappings;
						have_nt_file = true;
					} else if (nhdr.n_type == NT_TASKSTRUCT) {
						have_nt_taskstruct = true;
					}
				} else if (strncmp(name, "VMCOREINFO",
						   nhdr.n_namesz) == 0) {
					err = parse_vmcoreinfo(desc,
							       nhdr.n_descsz,
							       &vmcoreinfo);
					if (err)
						goto out_mappings;
					have_vmcoreinfo = true;
				}
			}
		}
	}
	elf_end(elf);
	elf = NULL;

	if (mappings_capacity > num_mappings) {
		/* We don't care if this fails. */
		resize_array(&mappings, num_mappings);
	}

	if (have_vmcoreinfo) {
		is_proc_kcore = true;
	} else if (have_nt_taskstruct) {
		/*
		 * Before Linux kernel commit 23c85094fe18 ("proc/kcore: add
		 * vmcoreinfo note to /proc/kcore") (in v4.19), /proc/kcore
		 * doesn't have a VMCOREINFO note. However, it has always had an
		 * NT_TASKSTRUCT note. If this is a file in /proc with the
		 * NT_TASKSTRUCT note, then it's probably /proc/kcore.
		 */
		struct statfs fs;

		if (fstatfs(fd, &fs) == -1) {
			err = drgn_error_create_os(errno, path, "fstatfs");
			if (err)
				goto out_mappings;
		}
		is_proc_kcore = fs.f_type == 0x9fa0; /* PROC_SUPER_MAGIC */
	} else {
		is_proc_kcore = false;
	}

	if (have_vmcoreinfo || is_proc_kcore) {
		/* Just in case the core dump also had any NT_FILE notes. */
		free_file_mappings(mappings, num_mappings);
		mappings = NULL;
		num_mappings = 0;

		/*
		 * Since Linux kernel commit 464920104bf7 ("/proc/kcore: update
		 * physical address for kcore ram and text") (in v4.11), we can
		 * read from the physical address of vmcoreinfo exported in
		 * sysfs. Before that, p_paddr in /proc/kcore is always zero, so
		 * we have to use a hackier fallback.
		 */
		if (!have_vmcoreinfo) {
			if (have_non_zero_phys_addr) {
				err = read_vmcoreinfo_from_sysfs(reader,
								 &vmcoreinfo);
			} else {
				err = get_fallback_vmcoreinfo(&vmcoreinfo);
			}
			if (err)
				goto out_mappings;
			have_vmcoreinfo = true;
		}
	} else if (!have_nt_file) {
		err = drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					"core dump has no NT_FILE or VMCOREINFO note");
		goto out_mappings;
	}

	err = drgn_dwarf_index_create(PROGRAM_DWARF_INDEX_FLAGS, &dindex);
	if (err)
		goto out_mappings;

	if (have_vmcoreinfo)
		err = open_kernel_files(dindex, vmcoreinfo.osrelease, verbose);
	else
		err = open_userspace_files(dindex, mappings, num_mappings);
	if (err)
		goto out_dindex;
	err = drgn_dwarf_index_update(dindex);
	if (err)
		goto out_dindex;

	err = drgn_type_index_create(drgn_dwarf_index_word_size(dindex),
				     drgn_dwarf_index_is_little_endian(dindex),
				     &tindex);
	if (err)
		goto out_dindex;

	err = drgn_dwarf_type_cache_create(tindex, dindex, &dtcache);
	if (err)
		goto out_tindex;

	err = drgn_type_index_add_finder(tindex, drgn_dwarf_type_find, dtcache);
	if (err)
		goto out_dtcache;

	err = drgn_dwarf_symbol_index_create(dtcache, &dsindex);
	if (err)
		goto out_dtcache;

	drgn_program_init(prog, reader, tindex, &dsindex->sindex);
	err = drgn_program_add_cleanup(prog, cleanup_fd, (void *)(intptr_t)fd);
	if (err)
		goto out_program;
	err = drgn_program_add_cleanup(prog, free, file_segments);
	if (err)
		goto out_cleanup_fd;
	err = drgn_program_add_cleanup(prog, cleanup_dwarf_index, dindex);
	if (err)
		goto out_cleanup_file_segments;
	err = drgn_program_add_cleanup(prog, cleanup_dwarf_type_cache, dtcache);
	if (err)
		goto out_cleanup_dindex;
	dsindex->prog = prog;
	if (have_vmcoreinfo) {
		prog->flags |= DRGN_PROGRAM_IS_LINUX_KERNEL;
		prog->vmcoreinfo = vmcoreinfo;
		dsindex->relocation_hook = kernel_relocation_hook;
	} else {
		prog->mappings = mappings;
		prog->num_mappings = num_mappings;
		dsindex->relocation_hook = userspace_relocation_hook;
		err = drgn_program_add_cleanup(prog, cleanup_file_mappings,
					       prog);
		if (err)
			goto out_cleanup_dtcache;
	}
	return NULL;

out_cleanup_dtcache:
	drgn_program_remove_cleanup(prog, cleanup_dwarf_type_cache, dtcache);
out_cleanup_dindex:
	drgn_program_remove_cleanup(prog, cleanup_dwarf_index, dindex);
out_cleanup_file_segments:
	drgn_program_remove_cleanup(prog, free, file_segments);
out_cleanup_fd:
	drgn_program_remove_cleanup(prog, cleanup_fd, (void *)(intptr_t)fd);
out_program:
	drgn_program_deinit(prog);
out_dtcache:
	drgn_dwarf_type_cache_destroy(dtcache);
out_tindex:
	drgn_type_index_destroy(tindex);
out_dindex:
	drgn_dwarf_index_destroy(dindex);
out_mappings:
	free_file_mappings(mappings, num_mappings);
	free(file_segments);
out_reader:
	drgn_memory_reader_destroy(reader);
out_elf:
	elf_end(elf);
out_fd:
	close(fd);
	return err;
}

struct drgn_error *drgn_program_init_kernel(struct drgn_program *prog,
					    bool verbose)
{
	return drgn_program_init_core_dump(prog, "/proc/kcore", verbose);
}

static struct drgn_error *parse_proc_maps(const char *maps_path,
					  struct file_mapping **mappings,
					  size_t *num_mappings)
{
	struct drgn_error *err;
	FILE *file;
	size_t capacity = 0;

	file = fopen(maps_path, "r");
	if (!file)
		return drgn_error_create_os(errno, maps_path, "fopen");

	for (;;) {
		unsigned long mapping_start, mapping_end;
		uint64_t file_offset;
		char *path;
		int ret;

		ret = fscanf(file, "%lx-%lx %*c%*c%*c%*c %" SCNx64 " "
			     "%*x:%*x %*d%*[ ]%m[^\n]", &mapping_start,
			     &mapping_end, &file_offset, &path);
		if (ret == EOF) {
			break;
		} else if (ret == 3) {
			/* This is an anonymous mapping; skip it. */
			continue;
		} else if (ret != 4) {
			err = drgn_error_format(DRGN_ERROR_OTHER,
						"could not parse %s", maps_path);
			goto out;
		}
		err = append_file_mapping(mapping_start, mapping_end,
					  file_offset, path, mappings,
					  num_mappings, &capacity);
		if (err && err->code == DRGN_ERROR_STOP) {
			/* The mapping was merged, so free the path. */
			free(path);
		} else if (err) {
			goto out;
		}
	}

	if (capacity > *num_mappings) {
		/* We don't care if this fails. */
		resize_array(mappings, *num_mappings);
	}

	err = NULL;
out:
	fclose(file);
	return err;
}

struct drgn_error *drgn_program_init_pid(struct drgn_program *prog, pid_t pid)
{
	struct drgn_error *err;
	char buf[64];
	int fd;
	struct drgn_memory_file_segment *file_segment;
	struct file_mapping *mappings = NULL;
	size_t num_mappings = 0;
	struct drgn_memory_reader *reader;
	struct drgn_dwarf_index *dindex;
	struct drgn_type_index *tindex;
	struct drgn_dwarf_type_cache *dtcache;
	struct drgn_dwarf_symbol_index *dsindex;

	sprintf(buf, "/proc/%ld/mem", (long)pid);
	fd = open(buf, O_RDONLY);
	if (fd == -1)
		return drgn_error_create_os(errno, buf, "open");

	file_segment = malloc(sizeof(*file_segment));
	if (!file_segment) {
		err = &drgn_enomem;
		goto out_fd;
	}
	file_segment->file_offset = 0;
	file_segment->file_size = UINT64_MAX;
	file_segment->fd = fd;

	err = drgn_memory_reader_create(&reader);
	if (err)
		goto out_file_segment;

	err = drgn_memory_reader_add_segment(reader, 0, UINT64_MAX, UINT64_MAX,
					     drgn_read_memory_file,
					     file_segment);
	if (err)
		goto out_reader;

	sprintf(buf, "/proc/%ld/maps", (long)pid);
	err = parse_proc_maps(buf, &mappings, &num_mappings);
	if (err)
		goto out_mappings;

	err = drgn_dwarf_index_create(PROGRAM_DWARF_INDEX_FLAGS, &dindex);
	if (err)
		goto out_mappings;
	err = open_userspace_files(dindex, mappings, num_mappings);
	if (err)
		goto out_dindex;
	err = drgn_dwarf_index_update(dindex);
	if (err)
		goto out_dindex;

	err = drgn_type_index_create(drgn_dwarf_index_word_size(dindex),
				     drgn_dwarf_index_is_little_endian(dindex),
				     &tindex);
	if (err)
		goto out_dindex;

	err = drgn_dwarf_type_cache_create(tindex, dindex, &dtcache);
	if (err)
		goto out_tindex;

	err = drgn_type_index_add_finder(tindex, drgn_dwarf_type_find, dtcache);
	if (err)
		goto out_dtcache;

	err = drgn_dwarf_symbol_index_create(dtcache, &dsindex);
	if (err)
		goto out_dtcache;

	drgn_program_init(prog, reader, tindex, &dsindex->sindex);
	dsindex->prog = prog;
	prog->mappings = mappings;
	prog->num_mappings = num_mappings;
	dsindex->relocation_hook = userspace_relocation_hook;
	err = drgn_program_add_cleanup(prog, cleanup_fd, (void *)(intptr_t)fd);
	if (err)
		goto out_program;
	err = drgn_program_add_cleanup(prog, free, file_segment);
	if (err)
		goto out_cleanup_fd;
	err = drgn_program_add_cleanup(prog, cleanup_dwarf_index, dindex);
	if (err)
		goto out_cleanup_file_segment;
	err = drgn_program_add_cleanup(prog, cleanup_dwarf_type_cache, dtcache);
	if (err)
		goto out_cleanup_dindex;
	err = drgn_program_add_cleanup(prog, cleanup_file_mappings, prog);
	if (err)
		goto out_cleanup_dtcache;
	return NULL;

out_cleanup_dtcache:
	drgn_program_remove_cleanup(prog, cleanup_dwarf_type_cache, dtcache);
out_cleanup_dindex:
	drgn_program_remove_cleanup(prog, cleanup_dwarf_index, dindex);
out_cleanup_file_segment:
	drgn_program_remove_cleanup(prog, free, file_segment);
out_cleanup_fd:
	drgn_program_remove_cleanup(prog, cleanup_fd, (void *)(intptr_t)fd);
out_program:
	drgn_program_deinit(prog);
out_dtcache:
	drgn_dwarf_type_cache_destroy(dtcache);
out_tindex:
	drgn_type_index_destroy(tindex);
out_dindex:
	drgn_dwarf_index_destroy(dindex);
out_mappings:
	free_file_mappings(mappings, num_mappings);
out_reader:
	drgn_memory_reader_destroy(reader);
out_file_segment:
	free(file_segment);
out_fd:
	close(fd);
	return err;
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_program_from_core_dump(const char *path, bool verbose,
			    struct drgn_program **ret)
{
	struct drgn_error *err;
	struct drgn_program *prog;

	prog = malloc(sizeof(*prog));
	if (!prog)
		return &drgn_enomem;

	err = drgn_program_init_core_dump(prog, path, verbose);
	if (err) {
		free(prog);
		return err;
	}

	*ret = prog;
	return NULL;
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_program_from_kernel(bool verbose, struct drgn_program **ret)
{
	struct drgn_error *err;
	struct drgn_program *prog;

	prog = malloc(sizeof(*prog));
	if (!prog)
		return &drgn_enomem;

	err = drgn_program_init_kernel(prog, verbose);
	if (err) {
		free(prog);
		return err;
	}

	*ret = prog;
	return NULL;
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_program_from_pid(pid_t pid, struct drgn_program **ret)
{
	struct drgn_error *err;
	struct drgn_program *prog;

	prog = malloc(sizeof(*prog));
	if (!prog)
		return &drgn_enomem;

	err = drgn_program_init_pid(prog, pid);
	if (err) {
		free(prog);
		return err;
	}

	*ret = prog;
	return NULL;
}

LIBDRGN_PUBLIC void drgn_program_destroy(struct drgn_program *prog)
{
	if (prog) {
		drgn_program_deinit(prog);
		free(prog);
	}
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_program_read_memory(struct drgn_program *prog, void *buf, uint64_t address,
			 size_t count, bool physical)
{
	return drgn_memory_reader_read(prog->reader, buf, address, count,
				       physical);
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_program_read_c_string(struct drgn_program *prog, uint64_t address,
			   bool physical, size_t max_size, char **ret)
{
	struct drgn_error *err;
	char *str;
	size_t size = 0, capacity = 1;

	str = malloc(capacity);
	if (!str)
		return &drgn_enomem;

	for (;;) {
		if (size >= capacity) {
			capacity *= 2;
			if (!resize_array(&str, capacity)) {
				free(str);
				return &drgn_enomem;
			}
		}

		if (size < max_size) {
			err = drgn_memory_reader_read(prog->reader, &str[size],
						      address, 1, physical);
			if (err) {
				free(str);
				return err;
			}
			if (!str[size++])
				break;
		} else {
			str[size++] = '\0';
			break;
		}
		address++;
	}
	/* We don't care if this fails. */
	resize_array(&str, size);
	*ret = str;
	return NULL;
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_program_find_type(struct drgn_program *prog, const char *name,
		       const char *filename, struct drgn_qualified_type *ret)
{
	return drgn_type_index_find(prog->tindex, name, filename,
				    &drgn_language_c, ret);
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_program_find_object(struct drgn_program *prog, const char *name,
			 const char *filename,
			 enum drgn_find_object_flags flags,
			 struct drgn_object *ret)
{
	struct drgn_error *err;
	struct drgn_symbol sym;

	if (ret->prog != prog) {
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "object is from wrong program");
	}

	err = drgn_symbol_index_find(prog->sindex, name, filename, flags, &sym);
	if (err)
		return err;
	if (sym.is_enumerator) {
		if (drgn_enum_type_is_signed(sym.qualified_type.type)) {
			return drgn_object_set_signed(ret, sym.qualified_type,
						      sym.svalue, 0);
		} else {
			return drgn_object_set_unsigned(ret, sym.qualified_type,
							sym.uvalue, 0);
		}
	} else {
		return drgn_object_set_reference(ret, sym.qualified_type,
						 sym.address, 0, 0,
						 sym.little_endian);
	}
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_program_element_info(struct drgn_program *prog, struct drgn_type *type,
			  struct drgn_element_info *ret)
{
	struct drgn_type *underlying_type;
	bool is_pointer, is_array;

	underlying_type = drgn_underlying_type(type);
	is_pointer = drgn_type_kind(underlying_type) == DRGN_TYPE_POINTER;
	is_array = drgn_type_kind(underlying_type) == DRGN_TYPE_ARRAY;
	if (!is_pointer && !is_array)
		return drgn_type_error("'%s' is not an array or pointer", type);

	ret->qualified_type = drgn_type_type(underlying_type);
	return drgn_type_bit_size(ret->qualified_type.type, &ret->bit_size);
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_program_member_info(struct drgn_program *prog, struct drgn_type *type,
			 const char *member_name, struct drgn_member_info *ret)
{
	struct drgn_error *err;
	struct drgn_member_value *member;

	err = drgn_type_index_find_member(prog->tindex, type, member_name,
					  strlen(member_name), &member);
	if (err)
		return err;

	err = drgn_lazy_type_evaluate(member->type, &ret->qualified_type);
	if (err)
		return err;
	ret->bit_offset = member->bit_offset;
	ret->bit_field_size = member->bit_field_size;
	return NULL;
}
