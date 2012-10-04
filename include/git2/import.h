/*
 * Copyright (C) 2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_git_import_h__
#define INCLUDE_git_import_h__

#include "common.h"
#include "types.h"
#include "oid.h"

/**
 * @file git2/import.h
 * @brief Git fast import routines
 * @defgroup git_import Git fast import routines
 * @ingroup Git
 * @{
 */
GIT_BEGIN_DECL

typedef struct git_importer git_importer;

GIT_EXTERN(int) git_importer_create(
	git_importer **importer_p,
	git_repository *repo);

GIT_EXTERN(int) git_importer_free(
	git_importer *importer);

GIT_EXTERN(int) git_importer_blob(
	git_oid *oid_p, git_importer *importer, const void *buffer, size_t len);

/** @} */
GIT_END_DECL
#endif
