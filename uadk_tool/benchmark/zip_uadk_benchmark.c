/* SPDX-License-Identifier: Apache-2.0 */

#include <numa.h>
#include "uadk_benchmark.h"

#include "zip_uadk_benchmark.h"
#include "include/wd_comp.h"
#include "include/wd_sched.h"
#include "include/fse.h"

#define ZIP_TST_PRT			printf
#define PATH_SIZE			64
#define ZIP_FILE			"./zip"
#define COMP_LEN_RATE			2
#define DECOMP_LEN_RATE			2
#define MAX_POOL_LENTH_COMP		1
#define COMPRESSION_RATIO_FACTOR	0.7
#define CHUNK_SIZE			(128 * 1024)
#define MAX_UNRECV_PACKET_NUM		2
struct uadk_bd {
	u8 *src;
	u8 *dst;
	u32 src_len;
	u32 dst_len;
};

struct bd_pool {
	struct uadk_bd *bds;
};

struct thread_pool {
	struct bd_pool *pool;
} g_zip_pool;

enum ZIP_OP_MODE {
	BLOCK_MODE,
	STREAM_MODE
};

enum ZIP_THREAD_STATE {
	THREAD_PROCESSING,
	THREAD_COMPLETED
};

struct zip_async_tag {
	handle_t sess;
	u32 td_id;
	u32 bd_idx;
	u32 cm_len;
	u32 recv_cnt;
	ZSTD_CCtx *cctx;
};

typedef struct uadk_thread_res {
	u32 alg;
	u32 mode;
	u32 optype;
	u32 td_id;
	u32 win_sz;
	u32 comp_lv;
	u32 send_cnt;
	struct zip_async_tag *tag;
	COMP_TUPLE_TAG *ftuple;
	char *hw_buff_out;
} thread_data;

struct zip_file_head {
	u32 file_size;
	u32 block_num;
	u32 blk_sz[MAX_POOL_LENTH_COMP];
};

static struct wd_ctx_config g_ctx_cfg;
static struct wd_sched *g_sched;
static struct sched_params param;
static unsigned int g_thread_num;
static unsigned int g_ctxnum;
static unsigned int g_pktlen;
static unsigned int g_prefetch;
static unsigned int g_state;

#ifndef ZLIB_FSE
static ZSTD_CCtx* zstd_soft_fse_init(unsigned    int level)
{
	return NULL;
}

static int zstd_soft_fse(void *Ftuple, ZSTD_inBuffer *input, ZSTD_outBuffer *output, ZSTD_CCtx * cctx, ZSTD_EndDirective cmode)
{
	return input->size;
}
#endif

static int save_file_data(const char *alg, u32 pkg_len, u32 optype)
{
	struct zip_file_head *fhead = NULL;
	char file_path[PATH_SIZE];
	u32 total_file_size = 0;
	double comp_rate = 0.0;
	u32 full_size;
	ssize_t size;
	int j, fd;
	int ret = 0;

	optype = optype % WD_DIR_MAX;
	if (optype != WD_DIR_COMPRESS) //compress
		return 0;

	ret = snprintf(file_path, PATH_SIZE, "%s_%u.%s", ZIP_FILE, pkg_len, alg);
	if (ret < 0)
		return -EINVAL;

	ret = access(file_path, F_OK);
	if (!ret) {
		ZIP_TST_PRT("compress data file: %s has exist!\n", file_path);
		return 0;
	}

	fd = open(file_path, O_WRONLY|O_CREAT, 0777);
	if (fd < 0) {
		ZIP_TST_PRT("compress data file open %s fail (%d)!\n", file_path, -errno);
		return -ENODEV;
	}

	fhead = malloc(sizeof(*fhead));
	if (!fhead) {
		ZIP_TST_PRT("failed to alloc file head memory\n");
		ret = -ENOMEM;
		goto fd_error;
	}

	// init file head informations
	for (j = 0; j < MAX_POOL_LENTH_COMP; j++) {
		fhead->blk_sz[j] = g_zip_pool.pool[0].bds[j].dst_len;
		total_file_size += fhead->blk_sz[j];
	}
	fhead->block_num = MAX_POOL_LENTH_COMP;
	fhead->file_size = total_file_size;
	size = write(fd, fhead, sizeof(*fhead));
	if (size < 0) {
		ZIP_TST_PRT("compress write file head failed: %lu!\n", size);
		ret = -EINVAL;
		goto write_error;
	}

	// write data for one buffer one buffer to file line.
	for (j = 0; j < MAX_POOL_LENTH_COMP; j++) {
		size = write(fd, g_zip_pool.pool[0].bds[j].dst,
				fhead->blk_sz[j]);
		if (size < 0) {
			ZIP_TST_PRT("compress write data error size: %lu!\n", size);
			ret = -ENODEV;
			break;
		}
	}

write_error:
	free(fhead);
fd_error:
	close(fd);

	full_size = g_pktlen * MAX_POOL_LENTH_COMP;
	comp_rate = (double) total_file_size / full_size;
	ZIP_TST_PRT("compress data rate: %.1f%%!\n", comp_rate * 100);

	return ret;
}

static int load_file_data(const char *alg, u32 pkg_len, u32 optype)
{
	struct zip_file_head *fhead = NULL;
	char file_path[PATH_SIZE];
	ssize_t size = 0xff;
	int i, j, fd;
	int ret;

	optype = optype % WD_DIR_MAX;
	if (optype != WD_DIR_DECOMPRESS) //decompress
		return 0;

	ret = snprintf(file_path, PATH_SIZE, "%s_%u.%s", ZIP_FILE, pkg_len, alg);
	if (ret < 0)
		return -EINVAL;

	ret = access(file_path, F_OK);
	if (ret) {
		ZIP_TST_PRT("Decompress data file: %s not exist!\n", file_path);
		return -EINVAL;
	}

	// read data from file
	fd = open(file_path, O_RDONLY, 0);
	if (fd < 0) {
		ZIP_TST_PRT("Decompress data file open %s fail (%d)!\n", file_path, -errno);
		return -ENODEV;
	}

	fhead = malloc(sizeof(*fhead));
	if (!fhead) {
		ZIP_TST_PRT("failed to alloc file head memory\n");
		ret = -ENOMEM;
		goto fd_err;
	}
	size = read(fd, fhead, sizeof(*fhead));
	if (size < 0 || fhead->block_num != MAX_POOL_LENTH_COMP) {
		ZIP_TST_PRT("failed to read file head\n");
		ret = -EINVAL;
		goto read_err;
	}

	// read data for one buffer one buffer from file line
	for (j = 0; j < MAX_POOL_LENTH_COMP; j++) {
		memset(g_zip_pool.pool[0].bds[j].src, 0x0,
			g_zip_pool.pool[0].bds[j].src_len);
		if (size != 0) { // zero size buffer no need to read;
			size = read(fd, g_zip_pool.pool[0].bds[j].src,
					fhead->blk_sz[j]);
			if (size < 0) {
				ZIP_TST_PRT("Decompress read data error size: %lu!\n", size);
				ret = -EINVAL;
				goto read_err;
			} else if (size == 0) {
				ZIP_TST_PRT("Read file to the end!");
			}
		}
		g_zip_pool.pool[0].bds[j].src_len = size;
	}

	for (i = 1; i < g_thread_num; i++) {
		for (j = 0; j < MAX_POOL_LENTH_COMP; j++) {
			if (g_zip_pool.pool[0].bds[j].src_len)
				memcpy(g_zip_pool.pool[i].bds[j].src,
					g_zip_pool.pool[0].bds[j].src,
					g_zip_pool.pool[0].bds[j].src_len);
			g_zip_pool.pool[i].bds[j].src_len =
				g_zip_pool.pool[0].bds[j].src_len;
		}
	}

read_err:
	free(fhead);
fd_err:
	close(fd);

	return ret;
}

static int zip_uadk_param_parse(thread_data *tddata, struct acc_option *options)
{
	u32 algtype = options->algtype;
	u32 optype = options->optype;
	u8 mode = BLOCK_MODE;
	u8 alg;

	if (optype >= WD_DIR_MAX << 1) {
		ZIP_TST_PRT("failed to get zip optype!\n");
		return -EINVAL;
	} else if (optype >= WD_DIR_MAX) {
		mode = STREAM_MODE;
		optype = optype % WD_DIR_MAX;
		options->optype = optype;
	}

	switch(algtype) {
	case ZLIB:
		alg = WD_ZLIB;
		break;
	case GZIP:
		alg = WD_GZIP;
		break;
	case DEFLATE:
		alg = WD_DEFLATE;
		break;
	case LZ77_ZSTD:
		alg = WD_LZ77_ZSTD;
		if (optype == WD_DIR_DECOMPRESS)
			ZIP_TST_PRT("Zip LZ77_ZSTD just support compress!\n");
		optype = WD_DIR_COMPRESS;
		break;
	default:
		ZIP_TST_PRT("failed to set zip alg\n");
		return -EINVAL;
	}

	tddata->alg = alg;
	tddata->mode = mode;
	tddata->optype = optype;
	tddata->win_sz = options->winsize;
	tddata->comp_lv = options->complevel;

	return 0;
}

static void uninit_ctx_config2(void)
{
	/* uninit2 */
	wd_comp_uninit2();
}

static int init_ctx_config2(struct acc_option *options)
{
	struct wd_ctx_params cparams = {0};
	struct wd_ctx_nums *ctx_set_num;
	int mode = options->syncmode;
	char alg_name[MAX_ALG_NAME];
	int ret;

	ret = get_alg_name(options->algtype, alg_name);
	if (ret) {
		ZIP_TST_PRT("failed to get valid alg name!\n");
		return -EINVAL;
	}

	ctx_set_num = calloc(WD_DIR_MAX, sizeof(*ctx_set_num));
	if (!ctx_set_num) {
		WD_ERR("failed to alloc ctx_set_size!\n");
		return -WD_ENOMEM;
	}

	cparams.op_type_num = WD_DIR_MAX;
	cparams.ctx_set_num = ctx_set_num;
	cparams.bmp = numa_allocate_nodemask();
	if (!cparams.bmp) {
		WD_ERR("failed to create nodemask!\n");
		ret = -WD_ENOMEM;
		goto out_freectx;
	}

	numa_bitmask_setall(cparams.bmp);

	for (int i = 0; i < WD_DIR_MAX; i++) {
		if (mode == CTX_MODE_SYNC)
			ctx_set_num[i].sync_ctx_num = g_ctxnum;
		else
			ctx_set_num[i].async_ctx_num = g_ctxnum;
	}

	/* init */
	ret = wd_comp_init2_(alg_name, SCHED_POLICY_RR, TASK_HW, &cparams);
	if (ret) {
		ZIP_TST_PRT("failed to do comp init2!\n");
		return ret;
	}

	return 0;

out_freectx:
	free(ctx_set_num);

	return ret;
}

static int specified_device_request_ctx(struct acc_option *options)
{
	struct uacce_dev_list *list = NULL;
	struct uacce_dev_list *tmp = NULL;
	char *alg = options->algclass;
	int mode = options->syncmode;
	struct uacce_dev *dev = NULL;
	int avail_ctx = 0;
	char *dev_name;
	int ret = 0;
	int i = 0;

	list = wd_get_accel_list(alg);
	if (!list) {
		ZIP_TST_PRT("failed to get %s device\n", alg);
		return -ENODEV;
	}

	for (tmp = list; tmp != NULL; tmp = tmp->next) {
		dev_name = strrchr(tmp->dev->dev_root, '/') + 1;
		if (!strcmp(dev_name, options->device)) {
			dev = tmp->dev;
			break;
		}
	}

	if (dev == NULL) {
		ZIP_TST_PRT("failed to find device %s\n", options->device);
		ret = -ENODEV;
		goto free_list;
	}

	avail_ctx = wd_get_avail_ctx(dev);
	if (avail_ctx < 0) {
		ZIP_TST_PRT("failed to get the number of available ctx from %s\n", options->device);
		ret = avail_ctx;
		goto free_list;
	} else if (avail_ctx < g_ctxnum) {
		ZIP_TST_PRT("error: not enough ctx available in %s\n", options->device);
		ret = -ENODEV;
		goto free_list;
	}

	/* If there is no numa, we default config to zero */
	if (dev->numa_id < 0)
		dev->numa_id = 0;

	for (; i < g_ctxnum; i++) {
		g_ctx_cfg.ctxs[i].ctx = wd_request_ctx(dev);
		if (!g_ctx_cfg.ctxs[i].ctx) {
			ZIP_TST_PRT("failed to alloc %dth ctx\n", i);
			ret = -ENOMEM;
			goto free_ctx;
		}
		g_ctx_cfg.ctxs[i].op_type = options->optype % WD_DIR_MAX;
		g_ctx_cfg.ctxs[i].ctx_mode = (__u8)mode;
	}

	wd_free_list_accels(list);
	return 0;

free_ctx:
	for (; i >= 0; i--)
		wd_release_ctx(g_ctx_cfg.ctxs[i].ctx);

free_list:
	wd_free_list_accels(list);

	return ret;
}

static int non_specified_device_request_ctx(struct acc_option *options)
{
	char *alg = options->algclass;
	int mode = options->syncmode;
	struct uacce_dev *dev = NULL;
	int ret = 0;
	int i = 0;

	while (i < g_ctxnum) {
		dev = wd_get_accel_dev(alg);
		if (!dev) {
			ZIP_TST_PRT("failed to get %s device\n", alg);
			ret = -ENODEV;
			goto free_ctx;
		}

		/* If there is no numa, we default config to zero */
		if (dev->numa_id < 0)
			dev->numa_id = 0;

		for (; i < g_ctxnum; i++) {
			g_ctx_cfg.ctxs[i].ctx = wd_request_ctx(dev);
			if (!g_ctx_cfg.ctxs[i].ctx)
				break;

			g_ctx_cfg.ctxs[i].op_type = options->optype % WD_DIR_MAX;
			g_ctx_cfg.ctxs[i].ctx_mode = (__u8)mode;
		}

		free(dev);
	}

	return 0;

free_ctx:
	for (; i >= 0; i--)
		wd_release_ctx(g_ctx_cfg.ctxs[i].ctx);

	return ret;
}

static int init_ctx_config(struct acc_option *options)
{
	int optype = options->optype;
	int mode = options->syncmode;
	int max_node;
	int ret = 0;

	optype = optype % WD_DIR_MAX;
	max_node = numa_max_node() + 1;
	if (max_node <= 0)
		return -EINVAL;

	memset(&g_ctx_cfg, 0, sizeof(struct wd_ctx_config));
	g_ctx_cfg.ctx_num = g_ctxnum;
	g_ctx_cfg.ctxs = calloc(g_ctxnum, sizeof(struct wd_ctx));
	if (!g_ctx_cfg.ctxs)
		return -ENOMEM;

	if (strlen(options->device) != 0)
		ret = specified_device_request_ctx(options);
	else
		ret = non_specified_device_request_ctx(options);

	if (ret) {
		ZIP_TST_PRT("failed to request zip ctx!\n");
		goto free_ctxs;
	}

	g_sched = wd_sched_rr_alloc(SCHED_POLICY_RR, 2, max_node, wd_comp_poll_ctx);
	if (!g_sched) {
		ZIP_TST_PRT("failed to alloc sched!\n");
		ret = -ENOMEM;
		goto free_ctx;
	}

	g_sched->name = SCHED_SINGLE;

	/*
	 * All contexts for 2 modes & 2 types.
	 * The test only uses one kind of contexts at the same time.
	 */
	param.numa_id = 0;
	param.type = optype;
	param.mode = mode;
	param.begin = 0;
	param.end = g_ctxnum - 1;
	ret = wd_sched_rr_instance(g_sched, &param);
	if (ret) {
		ZIP_TST_PRT("failed to fill sched data!\n");
		goto free_sched;
	}

	ret = wd_comp_init(&g_ctx_cfg, g_sched);
	if (ret) {
		ZIP_TST_PRT("failed to init zip ctx!\n");
		goto free_sched;
	}

	return 0;

free_sched:
	wd_sched_rr_release(g_sched);

free_ctx:
	for (int i = g_ctxnum; i >= 0; i--)
		wd_release_ctx(g_ctx_cfg.ctxs[i].ctx);

free_ctxs:
	free(g_ctx_cfg.ctxs);

	return ret;
}

static void uninit_ctx_config(void)
{
	int i;

	/* uninit */
	wd_comp_uninit();

	for (i = 0; i < g_ctx_cfg.ctx_num; i++)
		wd_release_ctx(g_ctx_cfg.ctxs[i].ctx);
	free(g_ctx_cfg.ctxs);
	wd_sched_rr_release(g_sched);
}

static int init_uadk_bd_pool(u32 optype)
{
	u32 outsize;
	u32 insize;
	int i, j;

	// make the block not align to 4K
	optype = optype % WD_DIR_MAX;
	if (optype == WD_DIR_COMPRESS) {//compress
		insize = g_pktlen;
		outsize = g_pktlen * COMP_LEN_RATE;
	} else { // decompress
		insize = g_pktlen;
		outsize = g_pktlen * DECOMP_LEN_RATE;
	}

	g_zip_pool.pool = malloc(g_thread_num * sizeof(struct bd_pool));
	if (!g_zip_pool.pool) {
		ZIP_TST_PRT("init uadk pool alloc thread failed!\n");
		return -ENOMEM;
	} else {
		for (i = 0; i < g_thread_num; i++) {
			g_zip_pool.pool[i].bds = malloc(MAX_POOL_LENTH_COMP *
							 sizeof(struct uadk_bd));
			if (!g_zip_pool.pool[i].bds) {
				ZIP_TST_PRT("init uadk bds alloc failed!\n");
				goto malloc_error1;
			}
			for (j = 0; j < MAX_POOL_LENTH_COMP; j++) {
				g_zip_pool.pool[i].bds[j].src = calloc(1, insize);
				if (!g_zip_pool.pool[i].bds[j].src)
					goto malloc_error2;
				g_zip_pool.pool[i].bds[j].src_len = insize;

				g_zip_pool.pool[i].bds[j].dst = malloc(outsize);
				if (!g_zip_pool.pool[i].bds[j].dst)
					goto malloc_error3;
				g_zip_pool.pool[i].bds[j].dst_len = outsize;

				get_rand_data(g_zip_pool.pool[i].bds[j].src, insize * COMPRESSION_RATIO_FACTOR);
				if (g_prefetch)
					get_rand_data(g_zip_pool.pool[i].bds[j].dst, outsize);
			}
		}
	}

	return 0;

malloc_error3:
	free(g_zip_pool.pool[i].bds[j].src);
malloc_error2:
	for (j--; j >= 0; j--) {
		free(g_zip_pool.pool[i].bds[j].src);
		free(g_zip_pool.pool[i].bds[j].dst);
	}
malloc_error1:
	for (i--; i >= 0; i--) {
		for (j = 0; j < MAX_POOL_LENTH_COMP; j++) {
			free(g_zip_pool.pool[i].bds[j].src);
			free(g_zip_pool.pool[i].bds[j].dst);
		}
		free(g_zip_pool.pool[i].bds);
		g_zip_pool.pool[i].bds = NULL;
	}
	free(g_zip_pool.pool);
	g_zip_pool.pool = NULL;

	ZIP_TST_PRT("init uadk bd pool alloc failed!\n");
	return -ENOMEM;
}

static void free_uadk_bd_pool(void)
{
	int i, j;

	for (i = 0; i < g_thread_num; i++) {
		if (g_zip_pool.pool[i].bds) {
			for (j = 0; j < MAX_POOL_LENTH_COMP; j++) {
				free(g_zip_pool.pool[i].bds[j].src);
				free(g_zip_pool.pool[i].bds[j].dst);
			}
		}
		free(g_zip_pool.pool[i].bds);
		g_zip_pool.pool[i].bds = NULL;
	}
	free(g_zip_pool.pool);
	g_zip_pool.pool = NULL;
}

/*-------------------------------uadk benchmark main code-------------------------------------*/
static void *zip_lz77_async_cb(struct wd_comp_req *req, void *data)
{
	struct zip_async_tag *tag = req->cb_param;
	struct bd_pool *uadk_pool;
	int td_id = tag->td_id;
	int idx = tag->bd_idx;
	ZSTD_inBuffer zstd_input;
	ZSTD_outBuffer zstd_output;
	ZSTD_CCtx *cctx = tag->cctx;
	size_t fse_size;

	uadk_pool = &g_zip_pool.pool[td_id];
	uadk_pool->bds[idx].dst_len = req->dst_len;

	zstd_input.src = req->src;
	zstd_input.size = req->src_len;
	zstd_input.pos = 0;
	zstd_output.dst = uadk_pool->bds[idx].dst;
	zstd_output.size = tag->cm_len;
	zstd_output.pos = 0;
	__atomic_add_fetch(&tag->recv_cnt, 1, __ATOMIC_RELAXED);
	fse_size = zstd_soft_fse(req->priv, &zstd_input, &zstd_output, cctx, ZSTD_e_end);

	uadk_pool->bds[idx].dst_len = fse_size;

	return NULL;
}

static void *zip_async_cb(struct wd_comp_req *req, void *data)
{
	struct zip_async_tag *tag = req->cb_param;
	struct bd_pool *uadk_pool;
	int td_id = tag->td_id;
	int idx = tag->bd_idx;
	__atomic_add_fetch(&tag->recv_cnt, 1, __ATOMIC_RELAXED);

	uadk_pool = &g_zip_pool.pool[td_id];
	uadk_pool->bds[idx].dst_len = req->dst_len;

	return NULL;
}

static void *zip_uadk_poll(void *data)
{
	thread_data *pdata = (thread_data *)data;
	u32 expt = ACC_QUEUE_SIZE * g_thread_num;
	u32 id = pdata->td_id;
	u32 count = 0;
	u32 recv = 0;
	int ret;

	if (id > g_ctxnum)
		return NULL;

	while (g_state == THREAD_PROCESSING) {
		ret = wd_comp_poll_ctx(id, expt, &recv);
		count += recv;
		recv = 0;
		if (unlikely(ret != -WD_EAGAIN && ret < 0)) {
			ZIP_TST_PRT("poll ret: %d!\n", ret);
			goto recv_error;
		}
	}

recv_error:
	add_recv_data(count, g_pktlen);

	return NULL;
}

static void *zip_uadk_poll2(void *data)
{
	u32 expt = ACC_QUEUE_SIZE * g_thread_num;
	u32 count = 0;
	u32 recv = 0;
	int  ret;

	while (g_state == THREAD_PROCESSING) {
		ret = wd_comp_poll(expt, &recv);
		count += recv;
		recv = 0;
		if (unlikely(ret != -WD_EAGAIN && ret < 0)) {
			ZIP_TST_PRT("poll ret: %d!\n", ret);
			goto recv_error;
		}
	}

recv_error:
	add_recv_data(count, g_pktlen);

	return NULL;
}

static void *zip_uadk_blk_lz77_sync_run(void *arg)
{
	thread_data *pdata = (thread_data *)arg;
	struct wd_comp_sess_setup comp_setup = {0};
	ZSTD_CCtx *cctx = zstd_soft_fse_init(15);
	ZSTD_inBuffer zstd_input = {0};
	ZSTD_outBuffer zstd_output = {0};
	COMP_TUPLE_TAG *ftuple = NULL;
	struct bd_pool *uadk_pool;
	struct wd_comp_req creq;
	char *hw_buff_out = NULL;
	size_t fse_size;
	handle_t h_sess;
	u32 first_len = 0;
	u32 out_len = 0;
	u32 count = 0;
	int ret, i;

	if (pdata->td_id > g_thread_num)
		return NULL;

	uadk_pool = &g_zip_pool.pool[pdata->td_id];
	memset(&comp_setup, 0, sizeof(comp_setup));
	memset(&creq, 0, sizeof(creq));

	comp_setup.alg_type = pdata->alg;
	comp_setup.op_type = pdata->optype;
	comp_setup.win_sz = pdata->win_sz;
	comp_setup.comp_lv = pdata->comp_lv;
	comp_setup.sched_param = &param;
	h_sess = wd_comp_alloc_sess(&comp_setup);
	if (!h_sess)
		return NULL;

	creq.op_type = pdata->optype;
	creq.src_len = g_pktlen;
	out_len = uadk_pool->bds[0].dst_len;

	creq.cb = NULL;
	creq.data_fmt = 0;
	creq.status = 0;

	ftuple = malloc(sizeof(COMP_TUPLE_TAG) * MAX_POOL_LENTH_COMP);
	if (!ftuple)
		goto fse_err;

	hw_buff_out = malloc(out_len * MAX_POOL_LENTH_COMP);
	if (!hw_buff_out)
		goto hw_buff_err;
	memset(hw_buff_out, 0x0, out_len * MAX_POOL_LENTH_COMP);

	while(1) {
		i = count % MAX_POOL_LENTH_COMP;
		creq.src = uadk_pool->bds[i].src;
		creq.dst = &hw_buff_out[i]; //temp out
		creq.src_len = uadk_pool->bds[i].src_len;
		creq.dst_len = out_len;
		creq.priv = &ftuple[i];

		ret = wd_do_comp_sync(h_sess, &creq);
		if (ret || creq.status)
			break;

		count++;
		zstd_input.src = creq.src;
		zstd_input.size = creq.src_len;
		zstd_input.pos = 0;
		zstd_output.dst = uadk_pool->bds[i].dst;
		zstd_output.size = out_len;
		zstd_output.pos = 0;
		fse_size = zstd_soft_fse(creq.priv, &zstd_input, &zstd_output, cctx, ZSTD_e_end);

		uadk_pool->bds[i].dst_len = fse_size;
		if (unlikely(i == 0))
			first_len = fse_size;
		if (get_run_state() == 0)
			break;
	}

hw_buff_err:
	free(hw_buff_out);
fse_err:
	free(ftuple);
	wd_comp_free_sess(h_sess);

	cal_avg_latency(count);
	if (pdata->optype == WD_DIR_COMPRESS)
		add_recv_data(count, g_pktlen);
	else
		add_recv_data(count, first_len);

	return NULL;
}

static void *zip_uadk_stm_lz77_sync_run(void *arg)
{
	thread_data *pdata = (thread_data *)arg;
	struct wd_comp_sess_setup comp_setup = {0};
	COMP_TUPLE_TAG *ftuple = NULL;
	struct bd_pool *uadk_pool;
	struct wd_comp_req creq;
	handle_t h_sess;
	void *src, *dst;
	u32 in_len = 0;
	u32 out_len = 0;
	u32 count = 0;
	int ret, i;

	if (pdata->td_id > g_thread_num)
		return NULL;

	uadk_pool = &g_zip_pool.pool[pdata->td_id];
	memset(&comp_setup, 0, sizeof(comp_setup));
	memset(&creq, 0, sizeof(creq));

	comp_setup.alg_type = pdata->alg;
	comp_setup.op_type = pdata->optype;
	comp_setup.win_sz = pdata->win_sz;
	comp_setup.comp_lv = pdata->comp_lv;
	comp_setup.sched_param = &param;
	h_sess = wd_comp_alloc_sess(&comp_setup);
	if (!h_sess)
		return NULL;

	creq.op_type = pdata->optype;
	out_len = uadk_pool->bds[0].dst_len;

	creq.cb = NULL;
	creq.data_fmt = 0;
	creq.status = 0;

	ftuple = malloc(sizeof(COMP_TUPLE_TAG) * MAX_POOL_LENTH_COMP);
	if (!ftuple)
		goto fse_err;

	while(1) {
		i = count % MAX_POOL_LENTH_COMP;
		src = uadk_pool->bds[i].src;
		dst = uadk_pool->bds[i].dst;
		in_len = uadk_pool->bds[0].src_len;
		out_len = uadk_pool->bds[0].dst_len;

		while (in_len > 0) {
			creq.src_len = in_len > CHUNK_SIZE ? CHUNK_SIZE : in_len;
			creq.dst_len = out_len > 2 * CHUNK_SIZE ? 2 * CHUNK_SIZE : out_len;
			creq.src = src;
			creq.dst = dst;
			creq.priv = &ftuple[i];

			ret = wd_do_comp_strm(h_sess, &creq);
			if (ret < 0 || creq.status == WD_IN_EPARA) {
				ZIP_TST_PRT("wd comp, invalid or incomplete data! "
				"ret(%d), req.status(%u)\n", ret, creq.status);
				break;
			}

			src += CHUNK_SIZE;
			in_len -= CHUNK_SIZE;
			dst += 2 * CHUNK_SIZE;
			out_len -= 2 * CHUNK_SIZE;
		}

		count++;

		if (get_run_state() == 0)
			break;
	}

	free(ftuple);
fse_err:
	wd_comp_free_sess(h_sess);

	cal_avg_latency(count);
	add_recv_data(count, g_pktlen);

	return NULL;
}

static void *zip_uadk_blk_lz77_async_run(void *arg)
{
	thread_data *pdata = (thread_data *)arg;
	struct wd_comp_sess_setup comp_setup = {0};
	ZSTD_CCtx *cctx = zstd_soft_fse_init(15);
	struct bd_pool *uadk_pool;
	struct wd_comp_req creq;
	handle_t h_sess;
	u32 out_len = 0;
	u32 count = 0;
	u32 try_cnt = 0;
	int ret, i;

	if (pdata->td_id > g_thread_num)
		return NULL;

	uadk_pool = &g_zip_pool.pool[pdata->td_id];
	memset(&comp_setup, 0, sizeof(comp_setup));
	memset(&creq, 0, sizeof(creq));

	comp_setup.alg_type = pdata->alg;
	comp_setup.op_type = pdata->optype;
	comp_setup.win_sz = pdata->win_sz;
	comp_setup.comp_lv = pdata->comp_lv;
	comp_setup.sched_param = &param;
	h_sess = wd_comp_alloc_sess(&comp_setup);
	if (!h_sess)
		return NULL;

	creq.op_type = pdata->optype;
	creq.src_len = g_pktlen;
	out_len = uadk_pool->bds[0].dst_len;

	creq.cb = zip_lz77_async_cb;
	creq.data_fmt = 0;
	creq.status = 0;

	while(1) {
		if (get_run_state() == 0)
			break;

		i = count % MAX_POOL_LENTH_COMP;
		creq.src = uadk_pool->bds[i].src;
		creq.dst = &pdata->hw_buff_out[i]; //temp out
		creq.src_len = uadk_pool->bds[i].src_len;
		creq.dst_len = out_len;
		creq.priv = &pdata->ftuple[i];

		pdata->tag[i].td_id = pdata->td_id;
		pdata->tag[i].bd_idx = i;
		pdata->tag[i].cm_len = out_len;
		pdata->tag[i].cctx = cctx;
		creq.cb_param = &pdata->tag[i];

		ret = wd_do_comp_async(h_sess, &creq);
		if (ret == -WD_EBUSY) {
			usleep(SEND_USLEEP * try_cnt);
			try_cnt++;
			if (try_cnt > MAX_TRY_CNT) {
				ZIP_TST_PRT("Test LZ77 compress send fail %d times!\n", MAX_TRY_CNT);
				try_cnt = 0;
			}
			continue;
		} else if (ret || creq.status) {
			break;
		}
		try_cnt = 0;
		count++;
		__atomic_add_fetch(&pdata->send_cnt, 1, __ATOMIC_RELAXED);
	}
	wd_comp_free_sess(h_sess);
	add_send_complete();

	return NULL;
}

static void *zip_uadk_blk_sync_run(void *arg)
{
	thread_data *pdata = (thread_data *)arg;
	struct wd_comp_sess_setup comp_setup = {0};
	struct bd_pool *uadk_pool;
	struct wd_comp_req creq;
	handle_t h_sess;
	u32 out_len = 0;
	u32 count = 0;
	int ret, i;

	if (pdata->td_id > g_thread_num)
		return NULL;

	uadk_pool = &g_zip_pool.pool[pdata->td_id];
	memset(&comp_setup, 0, sizeof(comp_setup));
	memset(&creq, 0, sizeof(creq));

	comp_setup.alg_type = pdata->alg;
	comp_setup.op_type = pdata->optype;
	comp_setup.win_sz = pdata->win_sz;
	comp_setup.comp_lv = pdata->comp_lv;
	comp_setup.sched_param = &param;
	h_sess = wd_comp_alloc_sess(&comp_setup);
	if (!h_sess)
		return NULL;

	creq.op_type = pdata->optype;
	creq.src_len = g_pktlen;
	out_len = uadk_pool->bds[0].dst_len;

	creq.cb = NULL;
	creq.data_fmt = 0;
	creq.priv = 0;
	creq.status = 0;

	while(1) {
		i = count % MAX_POOL_LENTH_COMP;
		creq.src = uadk_pool->bds[i].src;
		creq.dst = uadk_pool->bds[i].dst;
		creq.src_len = uadk_pool->bds[i].src_len;
		creq.dst_len = out_len;

		ret = wd_do_comp_sync(h_sess, &creq);
		if (ret || creq.status)
			break;

		count++;
		uadk_pool->bds[i].dst_len = creq.dst_len;
		if (get_run_state() == 0)
			break;
	}
	wd_comp_free_sess(h_sess);

	cal_avg_latency(count);
	add_recv_data(count, g_pktlen);

	return NULL;
}

static void *zip_uadk_stm_sync_run(void *arg)
{
	thread_data *pdata = (thread_data *)arg;
	struct wd_comp_sess_setup comp_setup = {0};
	struct bd_pool *uadk_pool;
	struct wd_comp_req creq;
	handle_t h_sess;
	u32 out_len = 0;
	u32 count = 0;
	int ret, i;

	if (pdata->td_id > g_thread_num)
		return NULL;

	uadk_pool = &g_zip_pool.pool[pdata->td_id];
	memset(&comp_setup, 0, sizeof(comp_setup));
	memset(&creq, 0, sizeof(creq));

	comp_setup.alg_type = pdata->alg;
	comp_setup.op_type = pdata->optype;
	comp_setup.win_sz = pdata->win_sz;
	comp_setup.comp_lv = pdata->comp_lv;
	comp_setup.sched_param = &param;
	h_sess = wd_comp_alloc_sess(&comp_setup);
	if (!h_sess)
		return NULL;

	creq.op_type = pdata->optype;
	creq.src_len = g_pktlen;
	out_len = uadk_pool->bds[0].dst_len;

	creq.cb = NULL;
	creq.data_fmt = 0;
	creq.priv = 0;
	creq.status = 0;

	while(1) {
		i = count % MAX_POOL_LENTH_COMP;
		creq.src = uadk_pool->bds[i].src;
		creq.dst = uadk_pool->bds[i].dst;
		creq.src_len = uadk_pool->bds[i].src_len;
		creq.dst_len = out_len;

		ret = wd_do_comp_sync2(h_sess, &creq);
		if (ret < 0 || creq.status == WD_IN_EPARA) {
			ZIP_TST_PRT("wd comp, invalid or incomplete data! "
			       "ret(%d), req.status(%u)\n", ret, creq.status);
			break;
		}

		count++;
		uadk_pool->bds[i].dst_len = creq.dst_len;

		if (get_run_state() == 0)
			break;
	}
	wd_comp_free_sess(h_sess);

	cal_avg_latency(count);
	add_recv_data(count, g_pktlen);

	return NULL;
}

static void *zip_uadk_blk_async_run(void *arg)
{
	thread_data *pdata = (thread_data *)arg;
	struct wd_comp_sess_setup comp_setup = {0};
	struct bd_pool *uadk_pool;
	struct wd_comp_req creq;
	handle_t h_sess;
	int try_cnt = 0;
	u32 out_len = 0;
	u32 count = 0;
	int ret, i;

	if (pdata->td_id > g_thread_num)
		return NULL;

	uadk_pool = &g_zip_pool.pool[pdata->td_id];
	memset(&comp_setup, 0, sizeof(comp_setup));
	memset(&creq, 0, sizeof(creq));

	comp_setup.alg_type = pdata->alg;
	comp_setup.op_type = pdata->optype;
	comp_setup.win_sz = pdata->win_sz;
	comp_setup.comp_lv = pdata->comp_lv;
	comp_setup.sched_param = &param;
	h_sess = wd_comp_alloc_sess(&comp_setup);
	if (!h_sess)
		return NULL;

	creq.op_type = pdata->optype;
	creq.src_len = g_pktlen;
	out_len = uadk_pool->bds[0].dst_len;

	creq.cb = zip_async_cb;
	creq.data_fmt = 0;
	creq.priv = 0;
	creq.status = 0;

	while(1) {
		if (get_run_state() == 0)
			break;

		i = count % MAX_POOL_LENTH_COMP;
		creq.src = uadk_pool->bds[i].src;
		creq.dst = uadk_pool->bds[i].dst;
		creq.src_len = uadk_pool->bds[i].src_len;
		creq.dst_len = out_len;

		pdata->tag[i].td_id = pdata->td_id;
		pdata->tag[i].bd_idx = i;
		creq.cb_param = &pdata->tag[i];

		ret = wd_do_comp_async(h_sess, &creq);
		if (ret == -WD_EBUSY) {
			usleep(SEND_USLEEP * try_cnt);
			try_cnt++;
			if (try_cnt > MAX_TRY_CNT) {
				ZIP_TST_PRT("Test compress send fail %d times!\n", MAX_TRY_CNT);
				try_cnt = 0;
			}
			continue;
		} else if (ret || creq.status) {
			break;
		}
		try_cnt = 0;
		count++;
		__atomic_add_fetch(&pdata->send_cnt, 1, __ATOMIC_RELAXED);
	}

	wd_comp_free_sess(h_sess);

	add_send_complete();

	return NULL;
}

static int zip_uadk_sync_threads(struct acc_option *options)
{
	typedef void *(*zip_sync_run)(void *arg);
	zip_sync_run uadk_zip_sync_run = NULL;
	thread_data threads_args[THREADS_NUM];
	thread_data threads_option;
	pthread_t tdid[THREADS_NUM];
	int i, ret;

	/* alg param parse and set to thread data */
	ret = zip_uadk_param_parse(&threads_option, options);
	if (ret)
		return ret;

	threads_option.optype = options->optype;
	if (threads_option.mode == 1) {// stream mode
		if (threads_option.alg == LZ77_ZSTD)
			uadk_zip_sync_run = zip_uadk_stm_lz77_sync_run;
		else
			uadk_zip_sync_run = zip_uadk_stm_sync_run;
	} else {
		if (threads_option.alg == LZ77_ZSTD)
			uadk_zip_sync_run = zip_uadk_blk_lz77_sync_run;
		else
			uadk_zip_sync_run = zip_uadk_blk_sync_run;
	}
	for (i = 0; i < g_thread_num; i++) {
		threads_args[i].alg = threads_option.alg;
		threads_args[i].mode = threads_option.mode;
		threads_args[i].optype = threads_option.optype;
		threads_args[i].win_sz = threads_option.win_sz;
		threads_args[i].comp_lv = threads_option.comp_lv;
		threads_args[i].td_id = i;
		ret = pthread_create(&tdid[i], NULL, uadk_zip_sync_run, &threads_args[i]);
		if (ret) {
			ZIP_TST_PRT("Create sync thread fail!\n");
			goto sync_error;
		}
	}

	/* join thread */
	for (i = 0; i < g_thread_num; i++) {
		ret = pthread_join(tdid[i], NULL);
		if (ret) {
			ZIP_TST_PRT("Join sync thread fail!\n");
			goto sync_error;
		}
	}

sync_error:
	return ret;
}

static int zip_uadk_async_threads(struct acc_option *options)
{
	typedef void *(*zip_async_run)(void *arg);
	zip_async_run uadk_zip_async_run = NULL;
	thread_data threads_args[THREADS_NUM];
	thread_data threads_option;
	pthread_t tdid[THREADS_NUM];
	pthread_t pollid[THREADS_NUM];
	int i, ret;

	/* alg param parse and set to thread data */
	ret = zip_uadk_param_parse(&threads_option, options);
	if (ret)
		return ret;

	if (threads_option.mode == STREAM_MODE) {// stream mode
		ZIP_TST_PRT("Stream mode can't support async mode!\n");
		return 0;
	}

	if (threads_option.alg == LZ77_ZSTD)
		uadk_zip_async_run = zip_uadk_blk_lz77_async_run;
	else
		uadk_zip_async_run = zip_uadk_blk_async_run;

	for (i = 0; i < g_ctxnum; i++) {
		threads_args[i].td_id = i;
		/* poll thread */
		if (options->inittype == INIT2_TYPE)
			ret = pthread_create(&pollid[i], NULL, zip_uadk_poll2, &threads_args[i]);
		else
			ret = pthread_create(&pollid[i], NULL, zip_uadk_poll, &threads_args[i]);
		if (ret) {
			ZIP_TST_PRT("Create poll thread fail!\n");
			goto async_error;
		}
	}

	for (i = 0; i < g_thread_num; i++) {
		threads_args[i].alg = threads_option.alg;
		threads_args[i].mode = threads_option.mode;
		threads_args[i].optype = threads_option.optype;
		threads_args[i].win_sz = threads_option.win_sz;
		threads_args[i].comp_lv = threads_option.comp_lv;
		threads_args[i].td_id = i;
		if (threads_option.alg == LZ77_ZSTD) {
			struct bd_pool *uadk_pool = &g_zip_pool.pool[i];
			u32 out_len = uadk_pool->bds[0].dst_len;

			threads_args[i].ftuple = malloc(sizeof(COMP_TUPLE_TAG) *
				MAX_POOL_LENTH_COMP);
			if (!threads_args[i].ftuple) {
				ZIP_TST_PRT("failed to malloc lz77 ftuple!\n");
				goto lz77_free;
			}

			threads_args[i].hw_buff_out = malloc(out_len * MAX_POOL_LENTH_COMP);
			if (!threads_args[i].hw_buff_out) {
				ZIP_TST_PRT("failed to malloc lz77 hw_buff_out!\n");
				goto lz77_free;
			}
			memset(threads_args[i].hw_buff_out, 0x0, out_len * MAX_POOL_LENTH_COMP);
		}
		threads_args[i].tag = malloc(sizeof(struct zip_async_tag) * MAX_POOL_LENTH_COMP);
		if (!threads_args[i].tag) {
			ZIP_TST_PRT("failed to malloc zip tag!\n");
			goto tag_free;
		}
		threads_args[i].tag->recv_cnt = 0;
		threads_args[i].send_cnt = 0;
		ret = pthread_create(&tdid[i], NULL, uadk_zip_async_run, &threads_args[i]);
		if (ret) {
			ZIP_TST_PRT("Create async thread fail!\n");
			goto tag_free;
		}
	}

	/* join thread */
	for (i = 0; i < g_thread_num; i++) {
		ret = pthread_join(tdid[i], NULL);
		if (ret) {
			ZIP_TST_PRT("Join async thread fail!\n");
			goto tag_free;
		}
	}

	/* wait for the poll to clear packets */
	g_state = THREAD_PROCESSING;
	for (i = 0; i < g_thread_num;) {
		if (threads_args[i].send_cnt <= threads_args[i].tag->recv_cnt + MAX_UNRECV_PACKET_NUM)
			i++;
	}
	g_state = THREAD_COMPLETED; // finish poll

	for (i = 0; i < g_ctxnum; i++) {
		ret = pthread_join(pollid[i], NULL);
		if (ret) {
			ZIP_TST_PRT("Join poll thread fail!\n");
			goto tag_free;
		}
	}

tag_free:
	for (i = 0; i < g_thread_num; i++) {
		if (threads_args[i].tag)
			free(threads_args[i].tag);
	}
lz77_free:
	if (threads_option.alg == LZ77_ZSTD) {
		for (i = 0; i < g_thread_num; i++) {
			if (threads_args[i].ftuple)
				free(threads_args[i].ftuple);

			if (threads_args[i].hw_buff_out)
				free(threads_args[i].hw_buff_out);
		}
	}
async_error:
	return ret;
}

int zip_uadk_benchmark(struct acc_option *options)
{
	u32 ptime;
	int ret;

	signal(SIGSEGV, segmentfault_handler);
	g_thread_num = options->threads;
	g_pktlen = options->pktlen;
	g_ctxnum = options->ctxnums;
	g_prefetch = options->prefetch;

	if (options->optype >= WD_DIR_MAX * 2) {
		ZIP_TST_PRT("ZIP optype error: %u\n", options->optype);
		return -EINVAL;
	}

	if (options->inittype == INIT2_TYPE)
		ret = init_ctx_config2(options);
	else
		ret = init_ctx_config(options);
	if (ret)
		return ret;

	ret = init_uadk_bd_pool(options->optype);
	if (ret)
		return ret;

	ret = load_file_data(options->algname, options->pktlen, options->optype);
	if (ret)
		return ret;

	get_pid_cpu_time(&ptime);
	time_start(options->times);
	if (options->syncmode)
		ret = zip_uadk_async_threads(options);
	else
		ret = zip_uadk_sync_threads(options);
	cal_perfermance_data(options, ptime);
	if (ret)
		return ret;

	ret = save_file_data(options->algname, options->pktlen, options->optype);
	if (ret)
		return ret;

	free_uadk_bd_pool();
	if (options->inittype == INIT2_TYPE)
		uninit_ctx_config2();
	else
		uninit_ctx_config();

	return 0;
}
