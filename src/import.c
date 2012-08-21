#include "git2/import.h"
#include "git2/blob.h"
#include "common.h"

struct git_importer {
	git_repository *owner;
};

int git_importer_create(
	git_importer **importer_p,
	git_repository *repo)
{
	git_importer *importer;

	assert(importer_p && repo);

	importer = git__calloc(1, sizeof(git_importer));
	GITERR_CHECK_ALLOC(importer);

	importer->owner = repo;

	*importer_p = importer;

	return 0;
}

int git_importer_free(git_importer *importer)
{
	git__free(importer);

	return 0;
}

int git_importer_blob(git_oid *oid_p, git_importer *importer, const void *buffer, size_t len)
{
	int error;

	assert(importer);

	error = git_blob_create_frombuffer(oid_p, importer->owner, buffer, len);

	return error;
}
