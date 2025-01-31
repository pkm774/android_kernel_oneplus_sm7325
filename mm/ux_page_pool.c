#include <asm/page.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/gfp.h>
#include <linux/mm_types.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <linux/cpufreq.h>
#include <linux/healthinfo/fg.h>
#include <linux/sched/rt.h>

#include "ux_page_pool.h"
#if defined(OPLUS_FEATURE_SCHED_ASSIST) && defined(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
#include <linux/sched_assist/sched_assist_common.h>
#endif /* OPLUS_FEATURE_SCHED_ASSIST */

#ifdef UXPAGEPOOL_DEBUG
#include <linux/cred.h>
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#endif

#define UX_PAGE_POOL_NAME "ux_page_pool_fillthread"
#define MAX_POOL_ALLOC_RETRIES (5)
#define K(x) ((x) << (PAGE_SHIFT - 10))
//static const unsigned int orders[] = {0, 4, 8};
static const unsigned int orders[] = {0, 1};
//32M for order 0, 8M  for order1 default
static const unsigned int page_pool_nr_pages[] = {(SZ_32M >> PAGE_SHIFT), (SZ_8M >> PAGE_SHIFT)};
#define NUM_ORDERS ARRAY_SIZE(orders)
static struct ux_page_pool *pools[NUM_ORDERS];
static struct task_struct *ux_page_pool_tsk = NULL;
static wait_queue_head_t kworkthread_waitq;
static unsigned int kworkthread_wait_flag;
static bool ux_page_pool_enabled = false;

#ifdef UXPAGEPOOL_DEBUG
static atomic_long_t g_alloc_pages_fast[NUM_ORDERS]
	[UX_POOL_MIGRATETYPE_TYPES_SIZE] = {{ATOMIC_LONG_INIT(0)}};
static atomic_long_t g_alloc_pages_fast_retry[NUM_ORDERS]
	[UX_POOL_MIGRATETYPE_TYPES_SIZE][MAX_POOL_ALLOC_RETRIES] = {{{ATOMIC_LONG_INIT(0)}}};
static atomic_long_t g_alloc_pages_slow[NUM_ORDERS]
	[UX_POOL_MIGRATETYPE_TYPES_SIZE] = {{ATOMIC_LONG_INIT(0)}};
static atomic_long_t fillthread_runtime_times[NUM_ORDERS]
	[UX_POOL_MIGRATETYPE_TYPES_SIZE] = {{ATOMIC_LONG_INIT(0)}};
#define PARA_BUF_LEN 128
#endif

static int page_pool_fill(struct ux_page_pool *pool, int migratetype);

bool get_critical_zeroslowpath_task_flag(struct task_struct *tsk)
{
	if ((tsk->ux_im_flag == IM_FLAG_SYSTEMSERVER_PID) ||
		(tsk->ux_im_flag == IM_FLAG_SURFACEFLINGER))
		return true;
	else
		return false;
}

bool is_critical_zeroslowpath_task(struct task_struct *tsk)
{
	if (unlikely(test_task_ux(current) || rt_task(current) || task_is_fg(current) ||
			get_critical_zeroslowpath_task_flag(current)))
		return true;
	else
		return false;
}

static int order_to_index(unsigned int order)
{
	int i;
	for (i = 0; i < NUM_ORDERS; i++) {
		if (order == orders[i])
			return i;
	}
	return -1;
}

static void page_pool_wakeup_process(struct ux_page_pool *pool)
{
	if (unlikely(!ux_page_pool_enabled))
		return;

	if (NULL == pool) {
		pr_err("%s: boost_pool is NULL!\n", __func__);
		return;
	}

	kworkthread_wait_flag = 1;
	wake_up_interruptible(&kworkthread_waitq);
}

void set_ux_page_pool_fillthread_cpus(void)
{
	struct cpumask mask;
	struct cpumask *cpumask = &mask;
	pg_data_t *pgdat = NODE_DATA(0);
	unsigned int cpu = 0, cpufreq_max_tmp = 0;
	struct cpufreq_policy *policy_max;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (policy == NULL)
			continue;

		if (policy->cpuinfo.max_freq >= cpufreq_max_tmp) {
			cpufreq_max_tmp = policy->cpuinfo.max_freq;
			policy_max = policy;
		}
	}

	cpumask_copy(cpumask, cpumask_of_node(pgdat->node_id));
	cpumask_andnot(cpumask, cpumask, policy_max->related_cpus);

	if (!cpumask_empty(cpumask)) {
		set_cpus_allowed_ptr(ux_page_pool_tsk, cpumask);
	}
}

static int ux_page_pool_fillthread(void *p)
{
	struct ux_page_pool *pool;
	int i, j;
	int ret;
#ifdef UXPAGEPOOL_DEBUG
	unsigned long begin;
	int record_i, record_j;
#endif

	if (unlikely(!ux_page_pool_enabled))
		return -1;
	/* FIXME:temporarily skip to avoid stability issues.
	set_ux_page_pool_fillthread_cpus();
	*/
	while (!kthread_should_stop()) {
		ret = wait_event_interruptible(kworkthread_waitq,
						       (kworkthread_wait_flag == 1));
		if (ret < 0)
			continue;

		kworkthread_wait_flag = 0;

		for (i = 0; i < NUM_ORDERS; i++) {
			pool = pools[i];
			for (j = 0; j < UX_POOL_MIGRATETYPE_TYPES_SIZE; j++) {
#ifdef UXPAGEPOOL_DEBUG
				if( pool->count[j] < pool->low[j]) {
					record_i = i;
					record_j = j;
				}

				begin = jiffies;
				pr_info("fill start >>>>>order:%d migratetype:%d low: %d high: %d \
					count:%d gfp_mask %#x.\n",
					pool->order, j, pool->low[j], pool->high[j], pool->count[j], \
					pool->gfp_mask);
#endif
				while (pool->count[j] < pool->high[j])
					page_pool_fill(pool, j);
#ifdef UXPAGEPOOL_DEBUG
				pr_info("fill end   <<<<<order:%d migratetype:%d low: %d high: %d \
					count:%d use %dms\n",
					pool->order, j, pool->low[j], pool->high[j],pool->count[j],
					jiffies_to_msecs(jiffies - begin));
#endif
			}
		}

#ifdef UXPAGEPOOL_DEBUG
		atomic_long_inc(&fillthread_runtime_times[record_i][record_j]);
#endif
	}
	return 0;
}

struct ux_page_pool *ux_page_pool_create(gfp_t gfp_mask, unsigned int order, unsigned int nr_pages)
{
	struct ux_page_pool *pool;
	int i;

	pool = kmalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return ERR_PTR(-ENOMEM);

	pool->gfp_mask = gfp_mask;
	pool->order = order;
	for (i = 0; i < UX_POOL_MIGRATETYPE_TYPES_SIZE; i++) {
		pool->count[i] = 0;
		/*MIGRATETYPE: UNMOVABLE & MOVABLE*/
		pool->high[i] = nr_pages  /  UX_POOL_MIGRATETYPE_TYPES_SIZE;
		/* wakeup kthread on count < low, low = high/2*/
		pool->low[i]  = pool->high[i]/2;
		INIT_LIST_HEAD(&pool->items[i]);

		pr_info("%s order:%d migratetype:%d low: %d high: %d count:%d.\n",
			__func__, pool->order, i, pool->low[i], pool->high[i], pool->count[i]);
	}

	spin_lock_init(&pool->lock);
	return pool;
}

static void page_pool_add(struct ux_page_pool *pool, struct page *page, int migratetype)
{
	unsigned long flags;
	spin_lock_irqsave(&pool->lock, flags);
	list_add_tail(&page->lru, &pool->items[migratetype]);
	pool->count[migratetype]++;
	spin_unlock_irqrestore(&pool->lock, flags);
}

static struct page *page_pool_remove(struct ux_page_pool *pool, int migratetype)
{
	struct page *page;
	unsigned long flags;
	spin_lock_irqsave(&pool->lock, flags);
	page = list_first_entry_or_null(&pool->items[migratetype], struct page, lru);
	if (page) {
		pool->count[migratetype]--;
		list_del(&page->lru);
	}
	spin_unlock_irqrestore(&pool->lock, flags);

	/* wakeup kthread on count < low*/
	if (pool->count[migratetype] < pool->low[migratetype])
		page_pool_wakeup_process(pool);

	return page;
}

static int page_pool_fill(struct ux_page_pool *pool, int migratetype)
{
	struct page *page;
	gfp_t gfp_refill = pool->gfp_mask;
	unsigned long pfn;

	if (NULL == pool) {
		pr_err("%s: pool is NULL!\n", __func__);
		return -EINVAL;
	}

	page = alloc_pages(gfp_refill, pool->order);
	if (NULL == page)
		return -ENOMEM;
	if (put_page_testzero(page)) {
		pfn = page_to_pfn(page);
		if (!free_unref_page_prepare2(page, pool->order, pfn)) {
			__free_pages(page, pool->order);
			pr_err("KEN_pages free_unref_page_prepare2 fail\n");
			return -EINVAL;
		}
	}

	page_pool_add(pool, page, migratetype);
	return true;
}

/* fast path */
struct page *ux_page_pool_alloc_pages(unsigned int order, int migratetype, bool may_retry)
{
	struct page *page = NULL;
	int retries = 0;
	struct ux_page_pool *pool = NULL;
	int order_ind = order_to_index(order);

	if (unlikely(!ux_page_pool_enabled) || (order_ind == -1) || \
		(migratetype >= UX_POOL_MIGRATETYPE_TYPES_SIZE))
		return NULL;

	pool = pools[order_ind];
	if (pool == NULL)
		return NULL;

	if (!(test_task_ux(current) || rt_task(current))) {
		if (pool->count[migratetype] < pool->low[migratetype])
			return NULL;
	}

retry:
	/* Fast-path: Get a page from cache */
	page = page_pool_remove(pool, migratetype);
	if (!page && may_retry && retries < MAX_POOL_ALLOC_RETRIES) {
		retries++;
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(msecs_to_jiffies(2));
		goto retry;
	}

#ifdef UXPAGEPOOL_DEBUG
	if (page) {
	    atomic_long_inc(&g_alloc_pages_fast[order][migratetype]);
	} else {
		atomic_long_inc(&g_alloc_pages_slow[order][migratetype]);
	}

	if(retries!=0)
		atomic_long_inc(&g_alloc_pages_fast_retry[order][migratetype][retries - 1]);
#endif

	return page;
}

int ux_page_pool_refill(struct page *page, unsigned int order, int migratetype)
{
	struct ux_page_pool *pool;
	int order_ind = order_to_index(order);

	if (unlikely(!ux_page_pool_enabled) || (order_ind == -1) || (migratetype \
		>= UX_POOL_MIGRATETYPE_TYPES_SIZE))
		return false;

	pool = pools[order_ind];
	if(pool == NULL)
		return false;

	if (pool->count[migratetype] >= pool->high[migratetype])
		return false;

	page_pool_add(pool, page, migratetype);
	return true;
}

#ifdef UXPAGEPOOL_DEBUG
static int ux_page_pool_show(struct seq_file *s, void *unused)
{
	int i, j, k;
	struct ux_page_pool *pool;

	for (i = 0; i < NUM_ORDERS; i++) {
		pool = pools[i];
		for (j = 0; j < UX_POOL_MIGRATETYPE_TYPES_SIZE; j++) {
			seq_printf(s,"order:%d migratetype:%d low: %d high: %d count:%d.\n",
				pool->order, j, pool->low[j], pool->high[j],pool->count[j]);
		}
	}

	//pool
	for (i = 0; i < NUM_ORDERS; i++) {
		for (j = 0; j < UX_POOL_MIGRATETYPE_TYPES_SIZE; j++) {
			seq_printf(s, "g_alloc_pages_fast[%d][%d]:%lld g_alloc_pages_slow[%d][%d]:\
				%lld fillthread_runtime_times:%d\n",
				i, j, atomic_long_read(&g_alloc_pages_fast[i][j]), i, j, \
					atomic_long_read(&g_alloc_pages_slow[i][j]), \
						atomic_long_read(&fillthread_runtime_times[i][j]));
				for (k = 0; k < MAX_POOL_ALLOC_RETRIES; k++)
					seq_printf (s,"g_alloc_pages_fast_retry[%d][%d][%d]:%lld\n",
						i, j, k, atomic_long_read\
							(&g_alloc_pages_fast_retry[i][j][k]));
			}
	}

	return 0;

}

static int ux_page_pool_open(struct inode *inode, struct file *file)
{
	return single_open(file, ux_page_pool_show, NULL);
}

static const struct file_operations ux_page_pool_fops = {
		.open		= ux_page_pool_open,
		.read		= seq_read,
		.llseek 	= seq_lseek,
		.release	= seq_release,
};
#endif /*UXPAGEPOOL_DEBUG*/


static int ux_page_pool_init(void)
{
	int i;
#ifdef UXPAGEPOOL_DEBUG
	struct proc_dir_entry *uxdebug_dir;
#endif

	for (i = 0; i < NUM_ORDERS; i++) {
		pools[i] = ux_page_pool_create((GFP_HIGHUSER | __GFP_ZERO | __GFP_NOWARN |
			   __GFP_NORETRY) & ~__GFP_RECLAIM, orders[i], page_pool_nr_pages[i]);
	}
	ux_page_pool_enabled = true;

	init_waitqueue_head(&kworkthread_waitq);
	ux_page_pool_tsk = kthread_run(ux_page_pool_fillthread, NULL, UX_PAGE_POOL_NAME);
	if (IS_ERR_OR_NULL(ux_page_pool_tsk)) {
		pr_err("%s:run ux_page_pool_fillthread failed!\n", __func__);
	}
	page_pool_wakeup_process(pools[0]);

#ifdef UXPAGEPOOL_DEBUG
	uxdebug_dir = proc_mkdir("oplus_uxdebug", NULL);
	proc_create("ux_page_pool", 0666, uxdebug_dir, &ux_page_pool_fops);
#endif

	return 0;
}

module_init(ux_page_pool_init);
