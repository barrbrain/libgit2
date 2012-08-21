#include "clar_libgit2.h"
#include "git2/import.h"

static git_repository *repo = NULL;

static char *test_data = "Hello, world.";

static int verify_blob_from_oid(
	const git_oid *oid_p,
	void *payload)
{
	git_blob *blob_p;
	int error = git_blob_lookup(&blob_p, repo, oid_p);
	if (error)
		return error;

	cl_assert(git_blob_rawsize(blob_p) == strlen((char *)payload));
	cl_assert(memcmp(payload, git_blob_rawcontent(blob_p), strlen((char *)payload)) == 0);

	git_blob_free(blob_p);

	return error;
}

void test_importer_blob__initialize(void)
{
	repo = cl_git_sandbox_init("empty_standard_repo");
}

void test_importer_blob__cleanup(void)
{
	cl_git_sandbox_cleanup();
}

void test_importer_blob__create_importer(void)
{
	git_importer *importer;

	cl_git_pass(git_importer_create(&importer, repo));

	cl_git_pass(git_importer_free(importer));
}

void test_importer_blob__basic(void)
{
	git_importer *importer;
	git_oid oid;

	cl_git_pass(git_importer_create(&importer, repo));

	cl_git_pass(git_importer_blob(&oid, importer, test_data, strlen(test_data)));

	cl_git_pass(
		verify_blob_from_oid(&oid, test_data));

	cl_git_pass(git_importer_free(importer));
}


void test_importer_blob__multiple(void)
{
	git_importer *importer;
	static char *data2 = "Some more data";
	static char *data3 = "Even more data";
	git_oid oid1, oid2, oid3;

	cl_git_pass(git_importer_create(&importer, repo));

	cl_git_pass(git_importer_blob(&oid1, importer, test_data, strlen(test_data)));

	cl_git_pass(git_importer_blob(&oid2, importer, data2, strlen(data2)));

	cl_git_pass(git_importer_blob(&oid3, importer, data3, strlen(data3)));

	cl_git_pass(
		verify_blob_from_oid(&oid1, test_data));

	cl_git_pass(
		verify_blob_from_oid(&oid2, data2));

	cl_git_pass(
		verify_blob_from_oid(&oid3, data3));

	cl_git_pass(git_importer_free(importer));
}

