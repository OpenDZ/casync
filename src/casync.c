#include <fcntl.h>
#include <gcrypt.h>
#include <sys/stat.h>

#include "cadecoder.h"
#include "caencoder.h"
#include "caindex.h"
#include "castore.h"
#include "casync.h"
#include "chunker.h"
#include "realloc-buffer.h"
#include "util.h"

typedef enum CaDirection {
        CA_SYNC_ENCODE,
        CA_SYNC_DECODE,
} CaDirection;

typedef struct CaSync {
        CaDirection direction;

        CaEncoder *encoder;
        CaDecoder *decoder;

        CaStore *wstore;
        CaStore **rstores;
        size_t n_rstores;

        CaChunker chunker;

        int base_fd;
        int archive_fd;

        char *base_path;
        char *archive_path;

        char *temporary_base_path;
        char *temporary_archive_path;

        mode_t base_mode;
        mode_t make_perm_mode;

        ReallocBuffer buffer;

        gcry_md_hd_t object_digest;
        gcry_md_hd_t archive_digest;

        CaIndex *index;

        bool eof;
} CaSync;

static CaSync *ca_sync_new(void) {
        CaSync *s;

        s = new0(CaSync, 1);
        if (!s)
                return NULL;

        s->base_fd = s->archive_fd = -1;
        s->base_mode = (mode_t) -1;
        s->make_perm_mode = (mode_t) -1;

        return s;
}

CaSync *ca_sync_new_encode(void) {
        CaSync *s;

        s = ca_sync_new();
        if (!s)
                return NULL;

        s->direction = CA_SYNC_ENCODE;
        return s;
}

CaSync *ca_sync_new_decode(void) {
        CaSync *s;

        s = ca_sync_new();
        if (!s)
                return NULL;

        s->direction = CA_SYNC_DECODE;
        return s;
}

CaSync *ca_sync_unref(CaSync *s) {
        size_t i;

        if (!s)
                return NULL;

        ca_encoder_unref(s->encoder);
        ca_decoder_unref(s->decoder);

        ca_store_unref(s->wstore);
        for (i = 0; i < s->n_rstores; i++)
                ca_store_unref(s->rstores[i]);
        free(s->rstores);

        safe_close(s->base_fd);
        safe_close(s->archive_fd);

        free(s->base_path);
        free(s->archive_path);

        if (s->temporary_base_path) {
                (void) unlink(s->temporary_base_path);
                free(s->temporary_base_path);
        }
        if (s->temporary_archive_path) {
                (void) unlink(s->temporary_archive_path);
                free(s->temporary_archive_path);
        }

        ca_index_unref(s->index);

        realloc_buffer_free(&s->buffer);

        gcry_md_close(s->object_digest);
        gcry_md_close(s->archive_digest);
        free(s);

        return NULL;
}

static int ca_sync_allocate_index(CaSync *s) {
        assert(s);

        if (s->index)
                return -EBUSY;

        if (s->direction == CA_SYNC_ENCODE)
                s->index = ca_index_new_write();
        else if (s->direction == CA_SYNC_DECODE)
                s->index = ca_index_new_read();
        else
                assert(false);

        if (!s->index)
                return -ENOMEM;

        return 0;
}

int ca_sync_set_index_fd(CaSync *s, int fd) {
        int r;

        if (!s)
                return -EINVAL;
        if (fd < 0)
                return -EINVAL;

        r = ca_sync_allocate_index(s);
        if (r < 0)
                return r;

        r = ca_index_set_fd(s->index, fd);
        if (r < 0) {
                s->index = ca_index_unref(s->index);
                return r;
        }

        return 0;
}

int ca_sync_set_index_path(CaSync *s, const char *path) {
        int r;

        if (!s)
                return -EINVAL;
        if (!path)
                return -EINVAL;

        r = ca_sync_allocate_index(s);
        if (r < 0)
                return r;

        r = ca_index_set_path(s->index, path);
        if (r < 0) {
                s->index = ca_index_unref(s->index);
                return r;
        }

        return 0;
}

int ca_sync_set_base_fd(CaSync *s, int fd) {
        if (!s)
                return -EINVAL;
        if (fd < 0)
                return -EINVAL;

        if (s->base_fd >= 0)
                return -EBUSY;
        if (s->base_mode != (mode_t) -1)
                return -EBUSY;
        if (s->base_path)
                return -EBUSY;

        s->base_fd = fd;
        return 0;
}

int ca_sync_set_base_path(CaSync *s, const char *path) {
        if (!s)
                return -EINVAL;
        if (!path)
                return -EINVAL;

        if (s->base_fd >= 0)
                return -EBUSY;
        if (s->base_path)
                return -EBUSY;

        s->base_fd = open(path, O_RDONLY|O_CLOEXEC|O_NOCTTY|O_DIRECTORY);
        if (s->base_fd >= 0) /* Base exists already and is a directory */
                return 0;

        if (s->direction == CA_SYNC_ENCODE) {
                if (errno != ENOTDIR)
                        return -errno;

                s->base_fd = open(path, O_RDONLY|O_CLOEXEC|O_NOCTTY);
                if (s->base_fd < 0)
                        return -errno;

                return 0;
        }

        assert(s->direction == CA_SYNC_DECODE);

        s->base_path = strdup(path);
        if (!s->base_path)
                return -ENOMEM;

        return 0;
}

int ca_sync_set_make_perm_mode(CaSync *s, mode_t m) {
        if (!s)
                return -EINVAL;
        if (m & ~(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH))
                return -EINVAL;
        if (s->direction != CA_SYNC_ENCODE)
                return -ENOTTY;

        if (s->make_perm_mode != (mode_t) -1)
                return -EBUSY;

        s->make_perm_mode = m;
        return 0;
}

int ca_sync_set_base_mode(CaSync *s, mode_t m) {
        if (!s)
                return -EINVAL;
        if (m & ~(07777 | S_IFMT))
                return -EINVAL;
        if (!S_ISREG(m) && !S_ISDIR(m) && !S_ISBLK(m))
                return -ENOTTY;
        if (s->direction == CA_SYNC_ENCODE)
                return -ENOTTY;

        if (s->base_fd >= 0)
                return -EBUSY;
        if (s->base_mode != (mode_t) -1)
                return -EBUSY;

        s->base_mode = m;
        return 0;
}

int ca_sync_set_archive_fd(CaSync *s, int fd) {
        if (!s)
                return -EINVAL;
        if (fd < 0)
                return -EINVAL;

        if (s->archive_fd >= 0)
                return -EBUSY;
        if (s->archive_path)
                return -EBUSY;

        s->archive_fd = fd;
        return 0;
}

int ca_sync_set_archive_path(CaSync *s, const char *path) {
        if (!s)
                return -EINVAL;
        if (!path)
                return -EINVAL;

        if (s->archive_fd >= 0)
                return -EBUSY;
        if (s->archive_path)
                return -EBUSY;

        if (s->direction == CA_SYNC_ENCODE) {
                s->archive_path = strdup(path);
                if (!s->archive_path)
                        return -ENOMEM;

                return 0;
        }

        assert(s->direction == CA_SYNC_DECODE);

        s->archive_fd = open(path, O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (s->archive_fd < 0)
                return -errno;

        return 0;
}

int ca_sync_set_store_local(CaSync *s, const char *path) {
        int r;

        if (!s)
                return -EINVAL;
        if (!path)
                return -EINVAL;

        if (s->wstore)
                return -EBUSY;

        s->wstore = ca_store_new();
        if (!s->wstore)
                return -ENOMEM;

        r = ca_store_set_local(s->wstore, path);
        if (r < 0) {
                s->wstore = ca_store_unref(s->wstore);
                return r;
        }

        return 0;
}

int ca_sync_add_seed_store_local(CaSync *s, const char *path) {
        CaStore **array, *store;
        int r;

        if (!s)
                return -EINVAL;
        if (!path)
                return -EINVAL;

        store = ca_store_new();
        if (!store)
                return -ENOMEM;

        r = ca_store_set_local(store, path);
        if (r < 0) {
                ca_store_unref(store);
                return r;
        }

        array = realloc(s->rstores, sizeof(CaStore*) * (s->n_rstores+1));
        if (!array) {
                ca_store_unref(store);
                return -ENOMEM;
        }

        s->rstores = array;
        s->rstores[s->n_rstores++] = store;

        return 0;
}

static int ca_sync_start(CaSync *s) {
        int r;

        assert(s);

        if (s->direction == CA_SYNC_ENCODE && s->archive_path && s->archive_fd < 0) {
                if (!s->temporary_archive_path) {
                        r = tempfn_random(s->archive_path, &s->temporary_archive_path);
                        if (r < 0)
                                return r;
                }

                s->archive_fd = open(s->temporary_archive_path, O_WRONLY|O_CLOEXEC|O_NOCTTY|O_CREAT|O_EXCL,
                                     s->make_perm_mode & 0666);
                if (s->archive_fd < 0) {
                        s->temporary_archive_path = mfree(s->temporary_archive_path);
                        return -errno;
                }
        }

        if (s->direction == CA_SYNC_ENCODE && !s->encoder) {

                if (s->base_fd < 0)
                        return -EUNATCH;

                s->encoder = ca_encoder_new();
                if (!s->encoder)
                        return -ENOMEM;

                r = ca_encoder_set_base_fd(s->encoder, s->base_fd);
                if (r < 0) {
                        s->encoder = ca_encoder_unref(s->encoder);
                        return r;
                }

                s->base_fd = -1;
        }

        if (s->direction == CA_SYNC_DECODE && !s->decoder) {

                if (s->base_fd < 0 && s->base_path) {

                        if (s->base_mode == (mode_t) -1)
                                return -EUNATCH;

                        if (S_ISDIR(s->base_mode)) {

                                if (mkdir(s->base_path, 0777) < 0 && errno != EEXIST)
                                        return -errno;

                                s->base_fd = open(s->base_path, O_RDONLY|O_CLOEXEC|O_NOCTTY|O_DIRECTORY);
                                if (s->base_fd < 0)
                                        return -errno;

                        } else if (S_ISREG(s->base_mode)) {

                                if (!s->temporary_base_path) {
                                        r = tempfn_random(s->base_path, &s->temporary_base_path);
                                        if (r < 0)
                                                return r;
                                }

                                s->base_fd = open(s->temporary_base_path, O_WRONLY|O_CLOEXEC|O_NOCTTY|O_CREAT|O_EXCL, 0777);
                                if (s->base_fd < 0) {
                                        s->temporary_base_path = mfree(s->temporary_base_path);
                                        return -errno;
                                }

                        } else {
                                assert(S_ISBLK(s->base_mode));

                                s->base_fd = open(s->base_path, O_WRONLY|O_CLOEXEC|O_NOCTTY);
                                if (s->base_fd < 0)
                                        return -errno;
                        }
                }

                s->decoder = ca_decoder_new();
                if (!s->decoder)
                        return -ENOMEM;

                if (s->base_fd >= 0) {

                        r = ca_decoder_set_base_fd(s->decoder, s->base_fd);
                        if (r < 0)
                                return r;

                        s->base_fd = -1;
                } else {

                        if (s->base_mode == (mode_t) -1)
                                return -EUNATCH;

                        r = ca_decoder_set_base_mode(s->decoder, s->base_mode);
                        if (r < 0)
                                return r;
                }
        }

        if (s->index) {
                r = ca_index_open(s->index);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int ca_sync_write_archive(CaSync *s, const void *p, size_t l) {
        assert(s);
        assert(p || l == 0);

        if (s->archive_fd < 0)
                return 0;

        return loop_write(s->archive_fd, p, l);
}

static void initialize_libgcrypt(void) {
        const char *p;

        if (gcry_control(GCRYCTL_INITIALIZATION_FINISHED_P))
                return;

        p = gcry_check_version("1.4.5");
        assert(p);

        gcry_control(GCRYCTL_DISABLE_SECMEM);
        gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
}

static int ca_sync_allocate_archive_digest(CaSync *s) {
        assert(s);

        if (s->archive_digest)
                return 0;

        initialize_libgcrypt();

        assert(gcry_md_get_algo_dlen(GCRY_MD_SHA256) == sizeof(ObjectID));

        if (gcry_md_open(&s->archive_digest, GCRY_MD_SHA256, 0) != 0)
                return -EIO;

        return 0;
}

static int ca_sync_write_archive_digest(CaSync *s, const void *p, size_t l) {
        int r;

        assert(s);
        assert(p || l == 0);

        r = ca_sync_allocate_archive_digest(s);
        if (r < 0)
                return r;

        gcry_md_write(s->archive_digest, p, l);
        return 0;
}

static int ca_sync_write_one_chunk(CaSync *s, const void *p, size_t l) {
        ObjectID id;
        int r;

        assert(s);
        assert(p || l == 0);

        r = ca_sync_make_object_id(s, p, l, &id);
        if (r < 0)
                return r;

        r = ca_store_put(s->wstore, &id, p, l);
        if (r < 0)
                return r;

        if (s->index)
                return ca_index_write_object(s->index, &id, l);

        return 0;
}

static int ca_sync_write_chunks(CaSync *s, const void *p, size_t l) {
        int r;

        assert(s);
        assert(p || l == 0);

        if (!s->wstore)
                return 0;

        while (l > 0) {
                const void *object;
                size_t object_size, k;

                k = ca_chunker_scan(&s->chunker, p, l);
                if (k == (size_t) -1) {
                        if (!realloc_buffer_append(&s->buffer, p, l))
                                return -ENOMEM;
                        return 0;
                }

                if (s->buffer.size == 0) {
                        object = p;
                        object_size = k;
                } else {
                        if (!realloc_buffer_append(&s->buffer, p, k))
                                return -ENOMEM;

                        object = s->buffer.data;
                        object_size = s->buffer.size;
                }

                r = ca_sync_write_one_chunk(s, object, object_size);
                if (r < 0)
                        return r;

                realloc_buffer_empty(&s->buffer);

                p = (const uint8_t*) p + k;
                l -= k;
        }

        return 0;
}

static int ca_sync_write_final_chunk(CaSync *s) {
        int r;

        assert(s);

        if (!s->wstore)
                return 0;

        if (s->buffer.size == 0)
                return 0;

        r = ca_sync_write_one_chunk(s, s->buffer.data, s->buffer.size);
        if (r < 0)
                return r;

        if (s->index) {
                unsigned char *q;

                r = ca_sync_allocate_archive_digest(s);
                if (r < 0)
                        return r;

                q = gcry_md_read(s->archive_digest, GCRY_MD_SHA256);
                if (!q)
                        return -EIO;

                r = ca_index_set_digest(s->index, (ObjectID*) q);
                if (r < 0)
                        return r;

                r = ca_index_write_eof(s->index);
                if (r < 0)
                        return r;

                r = ca_index_close(s->index);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int ca_sync_step_encode(CaSync *s) {
        int r;

        assert(s);

        if (s->eof)
                return -EPIPE;

        if (s->encoder) {
                int step;

                step = ca_encoder_step(s->encoder);
                if (step < 0)
                        return step;

                switch (step) {

                case CA_ENCODER_FINISHED:

                        if (s->temporary_archive_path && s->archive_path) {
                                if (rename(s->temporary_archive_path, s->archive_path) < 0)
                                        return -errno;

                                s->temporary_archive_path = mfree(s->temporary_archive_path);
                        }

                        r = ca_sync_write_final_chunk(s);
                        if (r < 0)
                                return r;

                        s->eof = true;

                        return CA_SYNC_FINISHED;

                case CA_ENCODER_NEXT_FILE:
                case CA_ENCODER_DATA: {
                        const void *p;
                        size_t l;

                        r = ca_encoder_get_data(s->encoder, &p, &l);
                        if (r < 0)
                                return r;

                        r = ca_sync_write_archive(s, p, l);
                        if (r < 0)
                                return r;

                        r = ca_sync_write_archive_digest(s, p, l);
                        if (r < 0)
                                return r;

                        r = ca_sync_write_chunks(s, p, l);
                        if (r < 0)
                                return r;

                        return step == CA_ENCODER_NEXT_FILE ? CA_SYNC_NEXT_FILE : CA_SYNC_STEP;
                }

                default:
                        assert(false);
                }
        } else
                assert(false);

        return 0;
}

static int ca_sync_process_decoder_request(CaSync *s) {
        int r;

        assert(s);

        if (s->index)  {
                uint64_t index_size, object_size;
                ObjectID id;
                void *p;

                r = ca_index_read_object(s->index, &id, &index_size);
                if (r < 0)
                        return r;
                if (r == 0) {
                        /* EOF */
                        r = ca_decoder_put_eof(s->decoder);
                        if (r < 0)
                                return r;

                        return 0;
                }

                r = ca_sync_get(s, &id, &p, &object_size);
                if (r < 0)
                        return r;

                if (index_size != object_size) {
                        free(p);
                        return -EBADMSG;
                }

                r = ca_decoder_put_data(s->decoder, p, index_size);
                free(p);
                if (r < 0)
                        return r;

                r = ca_sync_write_archive_digest(s, p, index_size);
                if (r < 0)
                        return r;

                return 0;
        }

        if (s->archive_fd >= 0) {
                r = ca_decoder_put_data_fd(s->decoder, s->archive_fd, UINT64_MAX, UINT64_MAX);
                if (r < 0)
                        return r;

                return 0;
        }

        return -ENOTTY;
}

static int ca_sync_step_decode(CaSync *s) {

        int r;

        assert(s);

        if (s->eof)
                return -EPIPE;

        if (s->decoder) {
                int step;

                step = ca_decoder_step(s->decoder);
                if (step < 0)
                        return step;

                switch (step) {

                case CA_DECODER_FINISHED:

                        if (s->temporary_base_path && s->base_path) {
                                if (rename(s->temporary_base_path, s->base_path) < 0)
                                        return -errno;

                                s->temporary_base_path = mfree(s->temporary_base_path);
                        }

                        s->eof = true;
                        return CA_SYNC_FINISHED;

                case CA_DECODER_NEXT_FILE:
                        return CA_SYNC_NEXT_FILE;

                case CA_DECODER_STEP:
                case CA_DECODER_PAYLOAD:
                        return CA_SYNC_STEP;

                case CA_DECODER_REQUEST:

                        r = ca_sync_process_decoder_request(s);
                        if (r < 0)
                                return r;

                        return CA_SYNC_STEP;
                }
        }

        return -ENOTTY;
}

int ca_sync_step(CaSync *s) {
        int r;

        if (!s)
                return -EINVAL;

        r = ca_sync_start(s);
        if (r < 0)
                return r;

        if (s->direction == CA_SYNC_ENCODE)
                return ca_sync_step_encode(s);
        else if (s->direction == CA_SYNC_DECODE)
                return ca_sync_step_decode(s);

        assert(false);
}

int ca_sync_get(CaSync *s, const ObjectID *object_id, void **ret, size_t *ret_size) {
        size_t i;
        int r;

        if (!s)
                return -EINVAL;
        if (!object_id)
                return -EINVAL;
        if (!ret)
                return -EINVAL;
        if (!ret_size)
                return -EINVAL;

        r = ca_store_get(s->wstore, object_id, ret, ret_size);
        if (r != -ENOENT)
                return r;

        for (i = 0; i < s->n_rstores; i++) {
                r = ca_store_get(s->rstores[i], object_id, ret, ret_size);
                if (r != -ENOENT)
                        return r;
        }

        return -ENOENT;
}

int ca_sync_put(CaSync *s, const ObjectID *object_id, const void *data, size_t size) {
        if (!s)
                return -EINVAL;
        if (!object_id)
                return -EINVAL;
        if (!data && size > 0)
                return -EINVAL;

        if (!s->wstore)
                return -EROFS;

        return ca_store_put(s->wstore, object_id, data, size);
}

int ca_sync_make_object_id(CaSync *s, const void *p, size_t l, ObjectID *ret) {
        unsigned char *q;

        if (!s)
                return -EINVAL;
        if (!p && l > 0)
                return -EINVAL;
        if (!ret)
                return -EINVAL;

        if (!s->object_digest) {
                initialize_libgcrypt();

                assert(gcry_md_get_algo_dlen(GCRY_MD_SHA256) == sizeof(ObjectID));

                if (gcry_md_open(&s->object_digest, GCRY_MD_SHA256, 0) != 0)
                        return -EIO;
        } else
                gcry_md_reset(s->object_digest);

        gcry_md_write(s->object_digest, p, l);

        q = gcry_md_read(s->object_digest, GCRY_MD_SHA256);
        if (!q)
                return -EIO;

        memcpy(ret, q, sizeof(ObjectID));
        return 0;
}

int ca_sync_get_digest(CaSync *s, ObjectID *ret) {
        unsigned char *q;
        int r;

        if (!s)
                return -EINVAL;
        if (!ret)
                return -EINVAL;

        if (!s->eof)
                return -EBUSY;

        r = ca_sync_allocate_archive_digest(s);
        if (r < 0)
                return r;

        q = gcry_md_read(s->archive_digest, GCRY_MD_SHA256);
        if (!q)
                return -EIO;

        memcpy(ret, q, sizeof(ObjectID));

        return 0;

}

int ca_sync_current_path(CaSync *s, char **ret) {
        if (!s)
                return -EINVAL;
        if (!ret)
                return -EINVAL;

        if (s->direction == CA_SYNC_ENCODE && s->encoder)
                return ca_encoder_current_path(s->encoder, ret);
        if (s->direction == CA_SYNC_DECODE && s->decoder)
                return ca_decoder_current_path(s->decoder, ret);

        return -ENOTTY;
}

int ca_sync_current_mode(CaSync *s, mode_t *ret) {
        if (!s)
                return -EINVAL;
        if (!ret)
                return -EINVAL;

        if (s->direction == CA_SYNC_ENCODE && s->encoder)
                return ca_encoder_current_mode(s->encoder, ret);
        if (s->direction == CA_SYNC_DECODE && s->decoder)
                return ca_decoder_current_mode(s->decoder, ret);

        return -ENOTTY;
}
