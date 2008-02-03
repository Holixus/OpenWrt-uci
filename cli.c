/*
 * Copyright (C) 2008 Felix Fietkau <nbd@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include "uci.h"

static const char *appname = "uci";
static enum {
	CLI_FLAG_MERGE = (1 << 0),
	CLI_FLAG_QUIET = (1 << 1)
} flags;
static FILE *input;

static struct uci_context *ctx;
enum {
	/* section cmds */
	CMD_GET,
	CMD_SET,
	CMD_DEL,
	CMD_RENAME,
	/* package cmds */
	CMD_SHOW,
	CMD_IMPORT,
	CMD_EXPORT,
	CMD_COMMIT,
};

static void uci_usage(int argc, char **argv)
{
	fprintf(stderr,
		"Usage: %s [<options>] <command> [<arguments>]\n\n"
		"Commands:\n"
		"\texport     [<config>]\n"
		"\timport     [<config>]\n"
		"\tshow       [<config>[.<section>[.<option>]]]\n"
		"\tget        <config>.<section>[.<option>]\n"
		"\tset        <config>.<section>[.<option>]=<value>\n"
		"\trename     <config>.<section>[.<option>]=<name>\n"
		"\n"
		"Options:\n"
		"\t-f <file>  use <file> as input instead of stdin\n"
		"\t-m         when importing, merge data into an existing package\n"
		"\t-n         name unnamed sections on export (default)\n"
		"\t-N         don't name unnamed sections\n"
		"\t-q         quiet mode (don't print error messages)\n"
		"\t-s         force strict mode (stop on parser errors, default)\n"
		"\t-S         disable strict mode\n"
		"\n",
		argv[0]
	);
	exit(255);
}

static void cli_perror(void)
{
	if (flags & CLI_FLAG_QUIET)
		return;

	uci_perror(ctx, appname);
}

static void uci_show_section(struct uci_section *p)
{
	struct uci_element *e;
	const char *cname, *sname;

	cname = p->package->e.name;
	sname = p->e.name;
	printf("%s.%s=%s\n", cname, sname, p->type);
	uci_foreach_element(&p->options, e) {
		printf("%s.%s.%s=%s\n", cname, sname, e->name, uci_to_option(e)->value);
	}
}

static void uci_show_package(struct uci_package *p)
{
	struct uci_element *e;

	uci_foreach_element( &p->sections, e) {
		uci_show_section(uci_to_section(e));
	}
}


static int package_cmd(int cmd, char *package)
{
	struct uci_package *p = NULL;

	if (uci_load(ctx, package, &p) != UCI_OK) {
		cli_perror();
		return 1;
	}
	if (!p)
		return 0;
	switch(cmd) {
	case CMD_COMMIT:
		if (uci_commit(ctx, &p, false) != UCI_OK)
			cli_perror();
		break;
	case CMD_EXPORT:
		uci_export(ctx, stdout, p, true);
		break;
	case CMD_SHOW:
		uci_show_package(p);
		break;
	}

	uci_unload(ctx, p);
	return 0;
}

static int uci_do_import(int argc, char **argv)
{
	struct uci_package *package = NULL;
	char *name = NULL;
	int ret = UCI_OK;

	if (argc > 2)
		return 255;

	if (argc == 2)
		name = argv[1];
	else if (flags & CLI_FLAG_MERGE)
		/* need a package to merge */
		return 255;

	if (flags & CLI_FLAG_MERGE) {
		if (uci_load(ctx, name, &package) != UCI_OK)
			package = NULL;
	}
	ret = uci_import(ctx, input, name, &package, (name != NULL));
	if (ret == UCI_OK) {
		if (flags & CLI_FLAG_MERGE) {
			ret = uci_save(ctx, package);
		} else {
			struct uci_element *e;
			/* loop through all config sections and overwrite existing data */
			uci_foreach_element(&ctx->root, e) {
				struct uci_package *p = uci_to_package(e);
				ret = uci_commit(ctx, &p, true);
			}
		}
	}

	if (ret != UCI_OK) {
		cli_perror();
		return 1;
	}

	return 0;
}

static int uci_do_package_cmd(int cmd, int argc, char **argv)
{
	char **configs = NULL;
	char **p;

	if (argc > 2)
		return 255;

	if (argc == 2)
		return package_cmd(cmd, argv[1]);

	if ((uci_list_configs(ctx, &configs) != UCI_OK) || !configs) {
		cli_perror();
		return 1;
	}

	for (p = configs; *p; p++) {
		package_cmd(cmd, *p);
	}

	return 0;
}


static int uci_do_section_cmd(int cmd, int argc, char **argv)
{
	struct uci_package *p = NULL;
	struct uci_element *e = NULL;
	char *package = NULL;
	char *section = NULL;
	char *option = NULL;
	char *value = NULL;
	char **ptr = NULL;
	int ret = UCI_OK;

	if (argc != 2)
		return 255;

	switch(cmd) {
	case CMD_SET:
	case CMD_RENAME:
		ptr = &value;
		break;
	default:
		break;
	}
	if (uci_parse_tuple(ctx, argv[1], &package, &section, &option, ptr) != UCI_OK)
		return 1;

	if (uci_load(ctx, package, &p) != UCI_OK) {
		cli_perror();
		return 1;
	}
	if (!p)
		return 0;

	switch(cmd) {
	case CMD_GET:
		if (uci_lookup(ctx, &e, p, section, option) != UCI_OK)
			return 1;

		switch(e->type) {
		case UCI_TYPE_SECTION:
			value = uci_to_section(e)->type;
			break;
		case UCI_TYPE_OPTION:
			value = uci_to_option(e)->value;
			break;
		default:
			/* should not happen */
			return 1;
		}
		/* throw the value to stdout */
		printf("%s\n", value);
		break;
	case CMD_RENAME:
		ret = uci_rename(ctx, p, section, option, value);
		break;
	case CMD_SET:
		ret = uci_set(ctx, p, section, option, value);
		break;
	case CMD_DEL:
		ret = uci_delete(ctx, p, section, option);
		break;
	}

	/* no save necessary for get */
	if (cmd == CMD_GET)
		return 0;

	/* save changes, but don't commit them yet */
	if (ret == UCI_OK)
		ret = uci_save(ctx, p);

	if (ret != UCI_OK) {
		cli_perror();
		return 1;
	}

	return 0;
}

static int uci_cmd(int argc, char **argv)
{
	int cmd = 0;

	if (!strcasecmp(argv[0], "show"))
		cmd = CMD_SHOW;
	else if (!strcasecmp(argv[0], "export"))
		cmd = CMD_EXPORT;
	else if (!strcasecmp(argv[0], "commit"))
		cmd = CMD_COMMIT;
	else if (!strcasecmp(argv[0], "get"))
		cmd = CMD_GET;
	else if (!strcasecmp(argv[0], "set"))
		cmd = CMD_SET;
	else if (!strcasecmp(argv[0], "ren") ||
	         !strcasecmp(argv[0], "rename"))
		cmd = CMD_RENAME;
	else if (!strcasecmp(argv[0], "del"))
		cmd = CMD_DEL;
	else if (!strcasecmp(argv[0], "import"))
		cmd = CMD_IMPORT;
	else
		cmd = -1;

	switch(cmd) {
		case CMD_GET:
		case CMD_SET:
		case CMD_DEL:
		case CMD_RENAME:
			return uci_do_section_cmd(cmd, argc, argv);
		case CMD_SHOW:
		case CMD_EXPORT:
		case CMD_COMMIT:
			return uci_do_package_cmd(cmd, argc, argv);
		case CMD_IMPORT:
			return uci_do_import(argc, argv);
		default:
			return 255;
	}
}

int main(int argc, char **argv)
{
	int ret;
	int c;

	input = stdin;
	ctx = uci_alloc_context();
	if (!ctx) {
		fprintf(stderr, "Out of memory\n");
		return 1;
	}

	while((c = getopt(argc, argv, "mf:sSnNq")) != -1) {
		switch(c) {
			case 'f':
				input = fopen(optarg, "r");
				if (!input) {
					perror("uci");
					return 1;
				}
				break;
			case 'm':
				flags |= CLI_FLAG_MERGE;
				break;
			case 's':
				ctx->flags |= UCI_FLAG_STRICT;
				break;
			case 'S':
				ctx->flags &= ~UCI_FLAG_STRICT;
				ctx->flags |= UCI_FLAG_PERROR;
				break;
			case 'n':
				ctx->flags |= UCI_FLAG_EXPORT_NAME;
				break;
			case 'N':
				ctx->flags &= ~UCI_FLAG_EXPORT_NAME;
				break;
			case 'q':
				flags |= CLI_FLAG_QUIET;
				break;
			default:
				uci_usage(argc, argv);
				break;
		}
	}
	if (optind > 1)
		argv[optind - 1] = argv[0];
	argv += optind - 1;
	argc -= optind - 1;

	if (argc < 2)
		uci_usage(argc, argv);
	ret = uci_cmd(argc - 1, argv + 1);
	if (input != stdin)
		fclose(input);
	if (ret == 255)
		uci_usage(argc, argv);

	uci_free_context(ctx);

	return ret;
}
