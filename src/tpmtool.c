/*
 * Copyright (C) 2010-2012 Free Software Foundation, Inc.
 *
 * Author: Nikos Mavrogiannopoulos
 *
 * This file is part of GnuTLS.
 *
 * GnuTLS is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuTLS is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <gnutls/openpgp.h>
#include <gnutls/pkcs12.h>
#include <gnutls/tpm.h>
#include <gnutls/abstract.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Gnulib portability files. */
#include <read-file.h>

#include "certtool-common.h"
#include "tpmtool-args.h"

static void cmd_parser(int argc, char **argv);
static void tpm_generate(FILE * outfile, unsigned int key_type,
			 unsigned int bits, unsigned int flags);
static void tpm_pubkey(const char *url, FILE * outfile);
static void tpm_delete(const char *url, FILE * outfile);
static void tpm_list(FILE * outfile);

static gnutls_x509_crt_fmt_t incert_format, outcert_format;
static gnutls_tpmkey_fmt_t inkey_format, outkey_format;

static FILE *outfile;
static FILE *infile;
int batch = 0;
int ask_pass = 0;

static void tls_log_func(int level, const char *str)
{
	fprintf(stderr, "|<%d>| %s", level, str);
}


int main(int argc, char **argv)
{
	cmd_parser(argc, argv);

	return 0;
}

static void cmd_parser(int argc, char **argv)
{
	int ret, debug = 0;
	unsigned int optct;
	unsigned int key_type = GNUTLS_PK_UNKNOWN;
	unsigned int bits = 0;
	unsigned int genflags = 0;
	/* Note that the default sec-param is legacy because several TPMs
	 * cannot handle larger keys.
	 */
	const char *sec_param = "legacy";

	optct = optionProcess(&tpmtoolOptions, argc, argv);
	argc += optct;
	argv += optct;

	if (HAVE_OPT(DEBUG))
		debug = OPT_VALUE_DEBUG;

	if (HAVE_OPT(INDER)) {
		incert_format = GNUTLS_X509_FMT_DER;
		inkey_format = GNUTLS_TPMKEY_FMT_DER;
	} else {
		incert_format = GNUTLS_X509_FMT_PEM;
		inkey_format = GNUTLS_TPMKEY_FMT_CTK_PEM;
	}

	if (HAVE_OPT(OUTDER)) {
		outcert_format = GNUTLS_X509_FMT_DER;
		outkey_format = GNUTLS_TPMKEY_FMT_DER;
	} else {
		outcert_format = GNUTLS_X509_FMT_PEM;
		outkey_format = GNUTLS_TPMKEY_FMT_CTK_PEM;
	}

	if (HAVE_OPT(REGISTER))
		genflags |= GNUTLS_TPM_REGISTER_KEY;
	if (!HAVE_OPT(LEGACY))
		genflags |= GNUTLS_TPM_KEY_SIGNING;
	if (HAVE_OPT(USER))
		genflags |= GNUTLS_TPM_KEY_USER;

	gnutls_global_set_log_function(tls_log_func);
	gnutls_global_set_log_level(debug);
	if (debug > 1)
		printf("Setting log level to %d\n", debug);

	if ((ret = gnutls_global_init()) < 0) {
		fprintf(stderr, "global_init: %s", gnutls_strerror(ret));
		exit(1);
	}

	if (HAVE_OPT(OUTFILE)) {
		outfile = safe_open_rw(OPT_ARG(OUTFILE), 0);
		if (outfile == NULL) {
			fprintf(stderr, "%s", OPT_ARG(OUTFILE));
			exit(1);
		}
	} else
		outfile = stdout;

	if (HAVE_OPT(INFILE)) {
		infile = fopen(OPT_ARG(INFILE), "rb");
		if (infile == NULL) {
			fprintf(stderr, "%s", OPT_ARG(INFILE));
			exit(1);
		}
	} else
		infile = stdin;

	if (HAVE_OPT(SEC_PARAM))
		sec_param = OPT_ARG(SEC_PARAM);
	if (HAVE_OPT(BITS))
		bits = OPT_VALUE_BITS;


	if (HAVE_OPT(GENERATE_RSA)) {
		key_type = GNUTLS_PK_RSA;
		bits = get_bits(key_type, bits, sec_param, 0);
		tpm_generate(outfile, key_type, bits, genflags);
	} else if (HAVE_OPT(PUBKEY)) {
		tpm_pubkey(OPT_ARG(PUBKEY), outfile);
	} else if (HAVE_OPT(DELETE)) {
		tpm_delete(OPT_ARG(DELETE), outfile);
	} else if (HAVE_OPT(LIST)) {
		tpm_list(outfile);
	} else {
		USAGE(1);
	}

	fclose(outfile);

	gnutls_global_deinit();
}

static void tpm_generate(FILE * out, unsigned int key_type,
			 unsigned int bits, unsigned int flags)
{
	int ret;
	char *srk_pass, *key_pass = NULL;
	gnutls_datum_t privkey, pubkey;

	srk_pass = getpass("Enter SRK password: ");
	if (srk_pass != NULL)
		srk_pass = strdup(srk_pass);

	if (!(flags & GNUTLS_TPM_REGISTER_KEY)) {
		key_pass = getpass("Enter key password: ");
		if (key_pass != NULL)
			key_pass = strdup(key_pass);
	}

	ret =
	    gnutls_tpm_privkey_generate(key_type, bits, srk_pass, key_pass,
					outkey_format, outcert_format,
					&privkey, &pubkey, flags);

	free(key_pass);
	free(srk_pass);

	if (ret < 0) {
		fprintf(stderr, "gnutls_tpm_privkey_generate: %s",
			gnutls_strerror(ret));
		exit(1);
	}

/*  fwrite (pubkey.data, 1, pubkey.size, outfile);
  fputs ("\n", outfile);*/
	fwrite(privkey.data, 1, privkey.size, out);
	fputs("\n", out);

	gnutls_free(privkey.data);
	gnutls_free(pubkey.data);
}

static void tpm_delete(const char *url, FILE * out)
{
	int ret;
	char *srk_pass;

	srk_pass = getpass("Enter SRK password: ");

	ret = gnutls_tpm_privkey_delete(url, srk_pass);
	if (ret < 0) {
		fprintf(stderr, "gnutls_tpm_privkey_delete: %s",
			gnutls_strerror(ret));
		exit(1);
	}

	fprintf(out, "Key %s deleted\n", url);
}

static void tpm_list(FILE * out)
{
	int ret;
	gnutls_tpm_key_list_t list;
	unsigned int i;
	char *url;

	ret = gnutls_tpm_get_registered(&list);
	if (ret < 0) {
		fprintf(stderr, "gnutls_tpm_get_registered: %s",
			gnutls_strerror(ret));
		exit(1);
	}

	fprintf(out, "Available keys:\n");
	for (i = 0;; i++) {
		ret = gnutls_tpm_key_list_get_url(list, i, &url, 0);
		if (ret == GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE)
			break;
		else if (ret < 0) {
			fprintf(stderr, "gnutls_tpm_key_list_get_url: %s",
				gnutls_strerror(ret));
			exit(1);
		}

		fprintf(out, "\t%u: %s\n", i, url);
		gnutls_free(url);
	}

	fputs("\n", out);
}

static void tpm_pubkey(const char *url, FILE * out)
{
	int ret;
	char *srk_pass;
	gnutls_pubkey_t pubkey;

	srk_pass = getpass("Enter SRK password: ");
	if (srk_pass != NULL)
		srk_pass = strdup(srk_pass);

	gnutls_pubkey_init(&pubkey);

	ret = gnutls_pubkey_import_tpm_url(pubkey, url, srk_pass, 0);

	free(srk_pass);

	if (ret < 0) {
		fprintf(stderr, "gnutls_pubkey_import_tpm_url: %s",
			gnutls_strerror(ret));
		exit(1);
	}

	_pubkey_info(out, GNUTLS_CRT_PRINT_FULL, pubkey);

	gnutls_pubkey_deinit(pubkey);
}
