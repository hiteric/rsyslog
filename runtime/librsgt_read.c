/* librsgt_read.c - rsyslog's guardtime support library
 * This includes functions used for reading signature (and 
 * other related) files.
 *
 * This part of the library uses C stdio and expects that the
 * caller will open and close the file to be read itself.
 *
 * Copyright 2013 Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <gt_http.h>

#include "librsgt.h"

typedef unsigned char uchar;
#ifndef VERSION
#define VERSION "no-version"
#endif
#define MAXFNAME 1024

static int rsgt_read_debug = 0;
char *rsgt_read_puburl = "http://verify.guardtime.com/gt-controlpublications.bin";
uint8_t rsgt_read_showVerified = 0;

/* macro to obtain next char from file including error tracking */
#define NEXTC	if((c = fgetc(fp)) == EOF) { \
			r = feof(fp) ? RSGTE_EOF : RSGTE_IO; \
			goto done; \
		}

/* check return state of operation and abort, if non-OK */
#define CHKr(code) if((r = code) != 0) goto done


/* if verbose==0, only the first and last two octets are shown,
 * otherwise everything.
 */
static void
outputHexBlob(FILE *fp, uint8_t *blob, uint16_t len, uint8_t verbose)
{
	unsigned i;
	if(verbose || len <= 8) {
		for(i = 0 ; i < len ; ++i)
			fprintf(fp, "%2.2x", blob[i]);
	} else {
		fprintf(fp, "%2.2x%2.2x%2.2x[...]%2.2x%2.2x%2.2x",
			blob[0], blob[1], blob[2],
			blob[len-3], blob[len-2], blob[len-1]);
	}
}

static inline void
outputHash(FILE *fp, char *hdr, uint8_t *data, uint16_t len, uint8_t verbose)
{
	fprintf(fp, "%s", hdr);
	outputHexBlob(fp, data, len, verbose);
	fputc('\n', fp);
}

void
rsgt_errctxInit(gterrctx_t *ectx)
{
	ectx->fp = NULL;
	ectx->filename = NULL;
	ectx->recNum = 0;
	ectx->recNumInFile = 0;
	ectx->blkNum = 0;
	ectx->verbose = 0;
	ectx->errRec = NULL;
	ectx->frstRecInBlk = NULL;
}
void
rsgt_errctxExit(gterrctx_t *ectx)
{
	free(ectx->filename);
	free(ectx->frstRecInBlk);
}

/* note: we do not copy the record, so the caller MUST not destruct
 * it before processing of the record is completed. To remove the
 * current record without setting a new one, call this function
 * with rec==NULL.
 */
void
rsgt_errctxSetErrRec(gterrctx_t *ectx, char *rec)
{
	ectx->errRec = strdup(rec);
}
/* This stores the block's first record. Here we copy the data,
 * as the caller will usually not preserve it long enough.
 */
void
rsgt_errctxFrstRecInBlk(gterrctx_t *ectx, char *rec)
{
	free(ectx->frstRecInBlk);
	ectx->frstRecInBlk = strdup(rec);
}

static void
reportError(int errcode, gterrctx_t *ectx)
{
	if(ectx->fp != NULL) {
		fprintf(ectx->fp, "%s[%llu:%llu:%llu]: error[%u]: %s\n",
			ectx->filename,
			(long long unsigned) ectx->blkNum, (long long unsigned) ectx->recNum,
			(long long unsigned) ectx->recNumInFile,
			errcode, RSGTE2String(errcode));
		if(ectx->frstRecInBlk != NULL)
			fprintf(ectx->fp, "\tBlock Start Record.: '%s'\n", ectx->frstRecInBlk);
		if(ectx->errRec != NULL)
			fprintf(ectx->fp, "\tRecord in Question.: '%s'\n", ectx->errRec);
		if(ectx->computedHash != NULL) {
			outputHash(ectx->fp, "\tComputed Hash......: ", ectx->computedHash->digest,
				ectx->computedHash->digest_length, ectx->verbose);
		}
		if(ectx->fileHash != NULL) {
			outputHash(ectx->fp, "\tSignature File Hash: ", ectx->fileHash->data,
				ectx->fileHash->len, ectx->verbose);
		}
		if(errcode == RSGTE_INVLD_TREE_HASH ||
		   errcode == RSGTE_INVLD_TREE_HASHID) {
			fprintf(ectx->fp, "\tTree Level.........: %d\n", (int) ectx->treeLevel);
			outputHash(ectx->fp, "\tTree Left Hash.....: ", ectx->lefthash->digest,
				ectx->lefthash->digest_length, ectx->verbose);
			outputHash(ectx->fp, "\tTree Right Hash....: ", ectx->righthash->digest,
				ectx->righthash->digest_length, ectx->verbose);
		}
		if(errcode == RSGTE_INVLD_TIMESTAMP ||
		   errcode == RSGTE_TS_DERDECODE) {
			fprintf(ectx->fp, "\tPublication Server.: %s\n", rsgt_read_puburl);
			fprintf(ectx->fp, "\tGT Verify Timestamp: [%u]%s\n",
				ectx->gtstate, GTHTTP_getErrorString(ectx->gtstate));
		}
	}
}

/* obviously, this is not an error-reporting function. We still use
 * ectx, as it has most information we need.
 */
static void
reportVerifySuccess(gterrctx_t *ectx)
{
	if(ectx->fp != NULL) {
		fprintf(ectx->fp, "%s[%llu:%llu:%llu]: block signature successfully verified\n",
			ectx->filename,
			(long long unsigned) ectx->blkNum, (long long unsigned) ectx->recNum,
			(long long unsigned) ectx->recNumInFile);
		if(ectx->frstRecInBlk != NULL)
			fprintf(ectx->fp, "\tBlock Start Record.: '%s'\n", ectx->frstRecInBlk);
		if(ectx->errRec != NULL)
			fprintf(ectx->fp, "\tBlock End Record...: '%s'\n", ectx->errRec);
		fprintf(ectx->fp, "\tGT Verify Timestamp: [%u]%s\n",
			ectx->gtstate, GTHTTP_getErrorString(ectx->gtstate));
	}
}


/**
 * Read a header from a binary file.
 * @param[in] fp file pointer for processing
 * @param[in] hdr buffer for the header. Must be 9 bytes 
 * 		  (8 for header + NUL byte)
 * @returns 0 if ok, something else otherwise
 */
int
rsgt_tlvrdHeader(FILE *fp, uchar *hdr)
{
	int r;
	if(fread(hdr, 8, 1, fp) != 1) {
		r = RSGTE_IO;
		goto done;
	}
	hdr[8] = '\0';
	r = 0;
done:	return r;
}

/* read type & length */
static int
rsgt_tlvrdTL(FILE *fp, uint16_t *tlvtype, uint16_t *tlvlen)
{
	int r = 1;
	int c;

	NEXTC;
	*tlvtype = c & 0x1f;
	if(c & 0x20) { /* tlv16? */
		NEXTC;
		*tlvtype = (*tlvtype << 8) | c;
		NEXTC;
		*tlvlen = c << 8;
		NEXTC;
		*tlvlen |= c;
	} else {
		NEXTC;
		*tlvlen = c;
	}
	if(rsgt_read_debug)
		printf("read tlvtype %4.4x, len %u\n", (unsigned) *tlvtype,
			(unsigned) *tlvlen);
	r = 0;
done:	return r;
}

static int
rsgt_tlvrdOctetString(FILE *fp, uint8_t **data, size_t len)
{
	size_t i;
	int c, r = 1;
	if((*data = (uint8_t*)malloc(len)) == NULL) {r=RSGTE_OOM;goto done;}
	for(i = 0 ; i < len ; ++i) {
		NEXTC;
		(*data)[i] = c;
	}
	r = 0;
done:	return r;
}
static int
rsgt_tlvrdSkipVal(FILE *fp, size_t len)
{
	size_t i;
	int c, r = 1;
	for(i = 0 ; i < len ; ++i) {
		NEXTC;
	}
	r = 0;
done:	return r;
}
static int
rsgt_tlvrdHASH_ALGO(FILE *fp, uint8_t *hashAlg)
{
	int r = 1;
	int c;
	uint16_t tlvtype, tlvlen;

	CHKr(rsgt_tlvrdTL(fp, &tlvtype, &tlvlen));
	if(!(tlvtype == 0x00 && tlvlen == 1)) {
		r = RSGTE_FMT;
		goto done;
	}
	NEXTC;
	*hashAlg = c;
	r = 0;
done:	return r;
}
static int
rsgt_tlvrdBLOCK_IV(FILE *fp, uint8_t **iv)
{
	int r = 1;
	uint16_t tlvtype, tlvlen;

	CHKr(rsgt_tlvrdTL(fp, &tlvtype, &tlvlen));
	if(!(tlvtype == 0x01)) {
		r = RSGTE_INVLTYP;
		goto done;
	}
	CHKr(rsgt_tlvrdOctetString(fp, iv, tlvlen));
	r = 0;
done:	return r;
}
static int
rsgt_tlvrdLAST_HASH(FILE *fp, imprint_t *imp)
{
	int r = 1;
	int c;
	uint16_t tlvtype, tlvlen;

	CHKr(rsgt_tlvrdTL(fp, &tlvtype, &tlvlen));
	if(!(tlvtype == 0x02)) { r = RSGTE_INVLTYP; goto done; }
	NEXTC;
	imp->hashID = c;
	if(tlvlen != 1 + hashOutputLengthOctets(imp->hashID)) {
		r = RSGTE_LEN;
		goto done;
	}
	imp->len = tlvlen - 1;
	CHKr(rsgt_tlvrdOctetString(fp, &imp->data, tlvlen-1));
	r = 0;
done:	return r;
}
static int
rsgt_tlvrdREC_COUNT(FILE *fp, uint64_t *cnt, size_t *lenInt)
{
	int r = 1;
	int i;
	int c;
	uint64_t val;
	uint16_t tlvtype, tlvlen;

	if((r = rsgt_tlvrdTL(fp, &tlvtype, &tlvlen)) != 0) goto done;
	if(!(tlvtype == 0x03 && tlvlen <= 8)) { r = RSGTE_INVLTYP; goto done; }
	*lenInt = tlvlen;
	val = 0;
	for(i = 0 ; i < tlvlen ; ++i) {
		NEXTC;
		val = (val << 8) + c;
	}
	*cnt = val;
	r = 0;
done:	return r;
}
static int
rsgt_tlvrdSIG(FILE *fp, block_sig_t *bs)
{
	int r = 1;
	uint16_t tlvtype, tlvlen;

	CHKr(rsgt_tlvrdTL(fp, &tlvtype, &tlvlen));
	if(!(tlvtype == 0x0906)) { r = RSGTE_INVLTYP; goto done; }
	bs->sig.der.len = tlvlen;
	bs->sigID = SIGID_RFC3161;
	CHKr(rsgt_tlvrdOctetString(fp, &(bs->sig.der.data), tlvlen));
	r = 0;
done:	return r;
}

static int
rsgt_tlvrdBLOCK_SIG(FILE *fp, block_sig_t **blocksig, uint16_t tlvlen)
{
	int r = 1;
	size_t lenInt = 0;
	uint16_t sizeRead;
	block_sig_t *bs;
	if((bs = calloc(1, sizeof(block_sig_t))) == NULL) {
		r = RSGTE_OOM;
		goto done;
	}
	CHKr(rsgt_tlvrdHASH_ALGO(fp, &(bs->hashID)));
	CHKr(rsgt_tlvrdBLOCK_IV(fp, &(bs->iv)));
	CHKr(rsgt_tlvrdLAST_HASH(fp, &(bs->lastHash)));
	CHKr(rsgt_tlvrdREC_COUNT(fp, &(bs->recCount), &lenInt));
	CHKr(rsgt_tlvrdSIG(fp, bs));
	sizeRead = 2 + 1 /* hash algo TLV */ +
	 	   2 + getIVLen(bs) /* iv */ +
		   2 + 1 + bs->lastHash.len /* last hash */ +
		   2 + lenInt /* rec-count */ +
		   4 + bs->sig.der.len /* rfc-3161 */;
	if(sizeRead != tlvlen) {
		r = RSGTE_LEN;
		goto done;
	}
	*blocksig = bs;
	r = 0;
done:	return r;
}

static int
rsgt_tlvrdIMPRINT(FILE *fp, imprint_t **imprint, uint16_t tlvlen)
{
	int r = 1;
	imprint_t *imp;
	int c;

	if((imp = calloc(1, sizeof(imprint_t))) == NULL) {
		r = RSGTE_OOM;
		goto done;
	}
	if((imp->data = calloc(1, sizeof(imprint_t))) == NULL) {
		r = RSGTE_OOM;
		goto done;
	}

	NEXTC;
	imp->hashID = c;
	if(tlvlen != 1 + hashOutputLengthOctets(imp->hashID)) {
		r = RSGTE_LEN;
		goto done;
	}
	imp->len = tlvlen - 1;
	CHKr(rsgt_tlvrdOctetString(fp, &imp->data, tlvlen-1));

	*imprint = imp;
	r = 0;
done:	return r;
}

static int
rsgt_tlvrdRecHash(FILE *fp, imprint_t **imp)
{
	int r;
	uint16_t tlvtype, tlvlen;

	if((r = rsgt_tlvrdTL(fp, &tlvtype, &tlvlen)) != 0) goto done;
	if(tlvtype != 0x0900) {
		r = RSGTE_MISS_REC_HASH;
		goto done;
	}
	if((r = rsgt_tlvrdIMPRINT(fp, imp, tlvlen)) != 0) goto done;
	r = 0;
done:	return r;
}

static int
rsgt_tlvrdTreeHash(FILE *fp, imprint_t **imp)
{
	int r;
	uint16_t tlvtype, tlvlen;

	if((r = rsgt_tlvrdTL(fp, &tlvtype, &tlvlen)) != 0) goto done;
	if(tlvtype != 0x0901) {
		r = RSGTE_MISS_TREE_HASH;
		goto done;
	}
	if((r = rsgt_tlvrdIMPRINT(fp, imp, tlvlen)) != 0) goto done;
	r = 0;
done:	return r;
}

/* read BLOCK_SIG during verification phase */
static int
rsgt_tlvrdVrfyBlockSig(FILE *fp, block_sig_t **bs)
{
	int r;
	uint16_t tlvtype, tlvlen;

	if((r = rsgt_tlvrdTL(fp, &tlvtype, &tlvlen)) != 0) goto done;
	if(tlvtype != 0x0902) {
		r = RSGTE_MISS_BLOCKSIG;
		goto done;
	}
	if((r = rsgt_tlvrdBLOCK_SIG(fp, bs, tlvlen)) != 0) goto done;
	r = 0;
done:	return r;
}

/**;
 * Read the next "object" from file. This usually is
 * a single TLV, but may be something larger, for
 * example in case of a block-sig TLV record.
 * Unknown type records are ignored (or run aborted
 * if we are not permitted to skip).
 *
 * @param[in] fp file pointer for processing
 * @param[out] tlvtype type of tlv record (top-level for
 * 		    structured objects.
 * @param[out] tlvlen length of the tlv record value
 * @param[out] obj pointer to object; This is a proper
 * 		   tlv record structure, which must be casted
 * 		   by the caller according to the reported type.
 * 		   The object must be freed by the caller (TODO: better way?)
 *
 * @returns 0 if ok, something else otherwise
 */
int
rsgt_tlvrd(FILE *fp, uint16_t *tlvtype, uint16_t *tlvlen, void *obj)
{
	int r = 1;

	if((r = rsgt_tlvrdTL(fp, tlvtype, tlvlen)) != 0) goto done;
	switch(*tlvtype) {
		case 0x0900:
			r = rsgt_tlvrdIMPRINT(fp, obj, *tlvlen);
			if(r != 0) goto done;
			break;
		case 0x0901:
			r = rsgt_tlvrdIMPRINT(fp, obj, *tlvlen);
			if(r != 0) goto done;
			break;
		case 0x0902:
			r = rsgt_tlvrdBLOCK_SIG(fp, obj, *tlvlen);
			if(r != 0) goto done;
			break;
	}
	r = 0;
done:	return r;
}

/* return if a blob is all zero */
static inline int
blobIsZero(uint8_t *blob, uint16_t len)
{
	int i;
	for(i = 0 ; i < len ; ++i)
		if(blob[i] != 0)
			return 0;
	return 1;
}

static void
rsgt_printIMPRINT(FILE *fp, char *name, imprint_t *imp, uint8_t verbose)
{
	fprintf(fp, "%s", name);
		outputHexBlob(fp, imp->data, imp->len, verbose);
		fputc('\n', fp);
}

static void
rsgt_printREC_HASH(FILE *fp, imprint_t *imp, uint8_t verbose)
{
	rsgt_printIMPRINT(fp, "[0x0900]Record hash: ",
		imp, verbose);
}

static void
rsgt_printINT_HASH(FILE *fp, imprint_t *imp, uint8_t verbose)
{
	rsgt_printIMPRINT(fp, "[0x0901]Tree hash..: ",
		imp, verbose);
}

/**
 * Output a human-readable representation of a block_sig_t
 * to proviced file pointer. This function is mainly inteded for
 * debugging purposes or dumping tlv files.
 *
 * @param[in] fp file pointer to send output to
 * @param[in] bsig ponter to block_sig_t to output
 * @param[in] verbose if 0, abbreviate blob hexdump, else complete
 */
void
rsgt_printBLOCK_SIG(FILE *fp, block_sig_t *bs, uint8_t verbose)
{
	fprintf(fp, "[0x0902]Block Signature Record:\n");
	fprintf(fp, "\tPrevious Block Hash:\n");
	fprintf(fp, "\t   Algorithm..: %s\n", hashAlgName(bs->lastHash.hashID));
	fprintf(fp, "\t   Hash.......: ");
		outputHexBlob(fp, bs->lastHash.data, bs->lastHash.len, verbose);
		fputc('\n', fp);
	if(blobIsZero(bs->lastHash.data, bs->lastHash.len))
		fprintf(fp, "\t   NOTE: New Hash Chain Start!\n");
	fprintf(fp, "\tHash Algorithm: %s\n", hashAlgName(bs->hashID));
	fprintf(fp, "\tIV............: ");
		outputHexBlob(fp, bs->iv, getIVLen(bs), verbose);
		fputc('\n', fp);
	fprintf(fp, "\tRecord Count..: %llu\n", bs->recCount);
	fprintf(fp, "\tSignature Type: %s\n", sigTypeName(bs->sigID));
	fprintf(fp, "\tSignature Len.: %u\n", bs->sig.der.len);
	fprintf(fp, "\tSignature.....: ");
		outputHexBlob(fp, bs->sig.der.data, bs->sig.der.len, verbose);
		fputc('\n', fp);
}


/**
 * Output a human-readable representation of a tlv object.
 *
 * @param[in] fp file pointer to send output to
 * @param[in] tlvtype type of tlv object (record)
 * @param[in] verbose if 0, abbreviate blob hexdump, else complete
 */
void
rsgt_tlvprint(FILE *fp, uint16_t tlvtype, void *obj, uint8_t verbose)
{
	switch(tlvtype) {
	case 0x0900:
		rsgt_printREC_HASH(fp, obj, verbose);
		break;
	case 0x0901:
		rsgt_printINT_HASH(fp, obj, verbose);
		break;
	case 0x0902:
		rsgt_printBLOCK_SIG(fp, obj, verbose);
		break;
	default:fprintf(fp, "unknown tlv record %4.4x\n", tlvtype);
		break;
	}
}


/**
 * Read block parameters. This detects if the block contains the
 * individual log hashes, the intermediate hashes and the overall
 * block paramters (from the signature block). As we do not have any
 * begin of block record, we do not know e.g. the hash algorithm or IV
 * until reading the block signature record. And because the file is
 * purely sequential and variable size, we need to read all records up to
 * the next signature record.
 * If a caller intends to verify a log file based on the parameters,
 * he must re-read the file from the begining (we could keep things
 * in memory, but this is impractical for large blocks). In order
 * to facitate this, the function permits to rewind to the original
 * read location when it is done.
 *
 * @param[in] fp file pointer of tlv file
 * @param[in] bRewind 0 - do not rewind at end of procesing, 1 - do so
 * @param[out] bs block signature record
 * @param[out] bHasRecHashes 0 if record hashes are present, 1 otherwise
 * @param[out] bHasIntermedHashes 0 if intermediate hashes are present,
 *                1 otherwise
 *
 * @returns 0 if ok, something else otherwise
 */
int
rsgt_getBlockParams(FILE *fp, uint8_t bRewind, block_sig_t **bs,
                    uint8_t *bHasRecHashes, uint8_t *bHasIntermedHashes)
{
	int r;
	uint64_t nRecs = 0;
	uint16_t tlvtype, tlvlen;
	uint8_t bDone = 0;
	off_t rewindPos = 0;

	if(bRewind)
		rewindPos = ftello(fp);
	*bHasRecHashes = 0;
	*bHasIntermedHashes = 0;
	*bs = NULL;

	while(!bDone) { /* we will err out on EOF */
		if((r = rsgt_tlvrdTL(fp, &tlvtype, &tlvlen)) != 0) goto done;
		switch(tlvtype) {
		case 0x0900:
			++nRecs;
			*bHasRecHashes = 1;
			rsgt_tlvrdSkipVal(fp, tlvlen);
			break;
		case 0x0901:
			*bHasIntermedHashes = 1;
			rsgt_tlvrdSkipVal(fp, tlvlen);
			break;
		case 0x0902:
			r = rsgt_tlvrdBLOCK_SIG(fp, bs, tlvlen);
			if(r != 0) goto done;
			bDone = 1;
			break;
		default:fprintf(fp, "unknown tlv record %4.4x\n", tlvtype);
			break;
		}
	}

	if(*bHasRecHashes && (nRecs != (*bs)->recCount)) {
		r = RSGTE_INVLD_RECCNT;
		goto done;
	}

	if(bRewind) {
		if(fseeko(fp, rewindPos, SEEK_SET) != 0) {
			r = RSGTE_IO;
			goto done;
		}
	}
done:
	return r;
}


/**
 * Read the file header and compare it to the expected value.
 * The file pointer is placed right after the header.
 * @param[in] fp file pointer of tlv file
 * @param[in] excpect expected header (e.g. "LOGSIG10")
 * @returns 0 if ok, something else otherwise
 */
int
rsgt_chkFileHdr(FILE *fp, char *expect)
{
	int r;
	char hdr[9];

	if((r = rsgt_tlvrdHeader(fp, (uchar*)hdr)) != 0) goto done;
	if(strcmp(hdr, expect))
		r = RSGTE_INVLHDR;
	else
		r = 0;
done:
	return r;
}

gtfile
rsgt_vrfyConstruct_gf(void)
{
	gtfile gf;
	if((gf = calloc(1, sizeof(struct gtfile_s))) == NULL)
		goto done;
	gf->x_prev = NULL;

done:	return gf;
}

void
rsgt_vrfyBlkInit(gtfile gf, block_sig_t *bs, uint8_t bHasRecHashes, uint8_t bHasIntermedHashes)
{
	gf->hashAlg = hashID2Alg(bs->hashID);
	gf->bKeepRecordHashes = bHasRecHashes;
	gf->bKeepTreeHashes = bHasIntermedHashes;
	free(gf->IV);
	gf->IV = malloc(getIVLen(bs));
	memcpy(gf->IV, bs->iv, getIVLen(bs));
	free(gf->blkStrtHash);
	gf->lenBlkStrtHash = bs->lastHash.len;
	gf->blkStrtHash = malloc(gf->lenBlkStrtHash);
	memcpy(gf->blkStrtHash, bs->lastHash.data, gf->lenBlkStrtHash);
}

static int
rsgt_vrfy_chkRecHash(gtfile gf, FILE *sigfp, GTDataHash *recHash, gterrctx_t *ectx)
{
	int r = 0;
	imprint_t *imp;

	if((r = rsgt_tlvrdRecHash(sigfp, &imp)) != 0)
		reportError(r, ectx);
		goto done;
	if(imp->hashID != hashIdentifier(gf->hashAlg)) {
		reportError(r, ectx);
		r = RSGTE_INVLD_REC_HASHID;
		goto done;
	}
	if(memcmp(imp->data, recHash->digest,
		  hashOutputLengthOctets(imp->hashID))) {
		r = RSGTE_INVLD_REC_HASH;
		ectx->computedHash = recHash;
		ectx->fileHash = imp;
		reportError(r, ectx);
		ectx->computedHash = NULL, ectx->fileHash = NULL;
		goto done;
	}
	r = 0;
done:
	return r;
}

static int
rsgt_vrfy_chkTreeHash(gtfile gf, FILE *sigfp, GTDataHash *hash, gterrctx_t *ectx)
{
	int r = 0;
	imprint_t *imp;

	if((r = rsgt_tlvrdTreeHash(sigfp, &imp)) != 0) {
		reportError(r, ectx);
		goto done;
	}
	if(imp->hashID != hashIdentifier(gf->hashAlg)) {
		reportError(r, ectx);
		r = RSGTE_INVLD_TREE_HASHID;
		goto done;
	}
	if(memcmp(imp->data, hash->digest,
		  hashOutputLengthOctets(imp->hashID))) {
		r = RSGTE_INVLD_TREE_HASH;
		ectx->computedHash = hash;
		ectx->fileHash = imp;
		reportError(r, ectx);
		ectx->computedHash = NULL, ectx->fileHash = NULL;
		goto done;
	}
	r = 0;
done:
	return r;
}

int
rsgt_vrfy_nextRec(block_sig_t *bs, gtfile gf, FILE *sigfp, unsigned char *rec,
	size_t len, gterrctx_t *ectx)
{
	int r = 0;
	GTDataHash *x; /* current hash */
	GTDataHash *m, *recHash, *t;
	uint8_t j;

	hash_m(gf, &m);
	hash_r(gf, &recHash, rec, len);
	if(gf->bKeepRecordHashes) {
		r = rsgt_vrfy_chkRecHash(gf, sigfp, recHash, ectx);
		if(r != 0) goto done;
	}
	hash_node(gf, &x, m, recHash, 1); /* hash leaf */
	if(gf->bKeepTreeHashes) {
		ectx->treeLevel = 0;
		ectx->lefthash = m;
		ectx->righthash = recHash;
		r = rsgt_vrfy_chkTreeHash(gf, sigfp, x, ectx);
		if(r != 0) goto done;
	}
	/* add x to the forest as new leaf, update roots list */
	t = x;
	for(j = 0 ; j < gf->nRoots ; ++j) {
		if(gf->roots_valid[j] == 0) {
			gf->roots_hash[j] = t;
			gf->roots_valid[j] = 1;
			t = NULL;
			break;
		} else if(t != NULL) {
			/* hash interim node */
			ectx->treeLevel = j+1;
			ectx->righthash = t;
			hash_node(gf, &t, gf->roots_hash[j], t, j+2);
			gf->roots_valid[j] = 0;
			if(gf->bKeepTreeHashes) {
				ectx->lefthash = gf->roots_hash[j];
				r = rsgt_vrfy_chkTreeHash(gf, sigfp, t, ectx);
				if(r != 0) goto done; /* mem leak ok, we terminate! */
			}
			GTDataHash_free(gf->roots_hash[j]);
		}
	}
	if(t != NULL) {
		/* new level, append "at the top" */
		gf->roots_hash[gf->nRoots] = t;
		gf->roots_valid[gf->nRoots] = 1;
		++gf->nRoots;
		assert(gf->nRoots < MAX_ROOTS);
		t = NULL;
	}
	gf->x_prev = x; /* single var may be sufficient */
	++gf->nRecords;

	/* cleanup */
	/* note: x is freed later as part of roots cleanup */
	GTDataHash_free(m);
	GTDataHash_free(recHash);
done:
	return r;
}


/* TODO: think about merging this with the writer. The
 * same applies to the other computation algos.
 */
static int
verifySigblkFinish(gtfile gf, GTDataHash **pRoot)
{
	GTDataHash *root, *rootDel;
	int8_t j;
	int r;

	if(gf->nRecords == 0)
		goto done;

	root = NULL;
	for(j = 0 ; j < gf->nRoots ; ++j) {
		if(root == NULL) {
			root = gf->roots_valid[j] ? gf->roots_hash[j] : NULL;
			gf->roots_valid[j] = 0; /* guess this is redundant with init, maybe del */
		} else if(gf->roots_valid[j]) {
			rootDel = root;
			hash_node(gf, &root, gf->roots_hash[j], root, j+2);
			gf->roots_valid[j] = 0; /* guess this is redundant with init, maybe del */
			GTDataHash_free(rootDel);
		}
	}

	free(gf->blkStrtHash);
	gf->blkStrtHash = NULL;
	*pRoot = root;
	r = 0;
done:
	gf->bInBlk = 0;
	return r;
}

/* verify the root hash. This also means we need to compute the
 * Merkle tree root for the current block.
 */
int
verifyBLOCK_SIG(block_sig_t *bs, gtfile gf, FILE *sigfp, gterrctx_t *ectx)
{
	int r;
	int gtstate;
	block_sig_t *file_bs;
	GTTimestamp *timestamp = NULL;
	GTVerificationInfo *vrfyInf;
	GTDataHash *root = NULL;
	
	if((r = verifySigblkFinish(gf, &root)) != 0)
		goto done;
	if((r = rsgt_tlvrdVrfyBlockSig(sigfp, &file_bs)) != 0)
		goto done;
	if(ectx->recNum != bs->recCount) {
		r = RSGTE_INVLD_RECCNT;
		goto done;
	}

	gtstate = GTTimestamp_DERDecode(file_bs->sig.der.data,
					file_bs->sig.der.len, &timestamp);
	if(gtstate != GT_OK) {
		r = RSGTE_TS_DERDECODE;
		ectx->gtstate = gtstate;
		goto done;
	}

	gtstate = GTHTTP_verifyTimestampHash(timestamp, root, NULL,
			NULL, NULL, rsgt_read_puburl, 0, &vrfyInf);
	if(! (gtstate == GT_OK
	      && vrfyInf->verification_errors == GT_NO_FAILURES) ) {
		r = RSGTE_INVLD_TIMESTAMP;
		ectx->gtstate = gtstate;
		goto done;
	}

	r = 0;
	if(rsgt_read_showVerified)
		reportVerifySuccess(ectx);
done:
	if(r != 0)
		reportError(r, ectx);
	if(timestamp != NULL)
		GTTimestamp_free(timestamp);
	return r;
}