// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/mm_types.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/page_ref.h>
#include <linux/gfp.h>
#include <linux/memblock.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/sysinfo.h>
#include <linux/nodemask.h>
#include <linux/fs.h>
#include <linux/pgtable.h>
#include <linux/pagemap.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CheUhxg");
MODULE_DESCRIPTION("Show memory statistics and process memory map");
MODULE_VERSION("2.0");

static int pid = -1;
module_param(pid, int, 0644);
MODULE_PARM_DESC(pid, "Target process PID");

static void traverse_all_pages(void)
{
    unsigned long total_pages = 0;//
    unsigned long valid_pages = 0;//
    unsigned long free_pages = 0;//？
    unsigned long anon_pages = 0;
    unsigned long file_pages = 0;//？
    unsigned long slab_pages = 0;
    unsigned long dirty_pages = 0;
    unsigned long writeback_pages = 0;
    unsigned long lru_pages = 0;
    int nid;

    pr_info("[memory_status] ===== Page Frame Traversal =====\n");

    for_each_online_node(nid) {
        unsigned long start_pfn = node_start_pfn(nid);
        unsigned long end_pfn   = node_end_pfn(nid);
        unsigned long pfn;

        for (pfn = start_pfn; pfn < end_pfn; pfn++) {
            struct page *page;

            total_pages++;

            if (!pfn_valid(pfn))
                continue;

            valid_pages++;
            page = pfn_to_page(pfn);

            /* TODO: classify page and update relevant counters */
            if (PageReserved(page))
                continue;
            if (page_count(page) == 0 && !PageReserved(page)) {
                free_pages++;
                continue;
            }
            if (PageSlab(page)){
                slab_pages++;
                continue;
            }
            if (PageAnon(page))
            anon_pages++;
            else if (!PageSwapCache(page) && page_mapping(page) != NULL)
                file_pages++;
            

            if (PageDirty(page))
                dirty_pages++;

            if (PageWriteback(page))
                writeback_pages++;

            if (PageLRU(page))
                lru_pages++;
        }
    }

    pr_info("[memory_status] ===== Page Type Summary =====\n");
    pr_info("[memory_status] total_pfn         = %lu\n", total_pages);
    pr_info("[memory_status] valid_pfn         = %lu\n", valid_pages);
    pr_info("[memory_status] free_pages        = %lu\n", free_pages);
    pr_info("[memory_status] anon_pages        = %lu\n", anon_pages);
    pr_info("[memory_status] file_pages        = %lu\n", file_pages);
    pr_info("[memory_status] slab_pages        = %lu\n", slab_pages);
    pr_info("[memory_status] dirty_pages       = %lu\n", dirty_pages);
    pr_info("[memory_status] writeback_pages   = %lu\n", writeback_pages);
    pr_info("[memory_status] lru_pages         = %lu\n", lru_pages);
    pr_info("[memory_status] PAGE_SIZE         = %lu bytes\n", PAGE_SIZE);
}

static void show_vmas(struct mm_struct *mm)
{
    struct vm_area_struct *vma;

    pr_info("[memory_status] ===== Traverse VMA =====\n");

    /* TODO: iterate VMAs and print basic info */
    for (vma = mm->mmap; vma; vma = vma->vm_next) {
        const char *filename;

        if (vma->vm_file)
            filename = vma->vm_file->f_path.dentry->d_name.name;
        else
            filename = "(null)";

        pr_info("VMA: 0x%lx - 0x%lx, flags=0x%lx, anon=%d, file=%s\n",
                vma->vm_start,
                vma->vm_end,
                vma->vm_flags,
                vma_is_anonymous(vma),
                filename);
    }

}

static void traverse_page_table(struct mm_struct *mm)
{
    unsigned long addr;
    unsigned long mapped_pages = 0;
    const unsigned long end = mm->task_size;

    pr_info("[memory_status] ===== Page Table Walk (partial) =====\n");

    for (addr = 0; addr < end; addr += PAGE_SIZE) {
        /* TODO: walk the page table and count mapped pages
         * Reminder: use pte_offset_map() to access PTEs safely
         */
        pgd_t *pgd;
        p4d_t *p4d;
        pud_t *pud;
        pmd_t *pmd;
        pte_t *pte;

        unsigned long pfn;
        phys_addr_t pa;

        pgd = pgd_offset(mm, addr);
        if (pgd_none(*pgd) || pgd_bad(*pgd))
            continue;

        p4d = p4d_offset(pgd, addr);
        if (p4d_none(*p4d) || p4d_bad(*p4d))
            continue;

        pud = pud_offset(p4d, addr);
        if (pud_none(*pud) || pud_bad(*pud))
            continue;

        pmd = pmd_offset(pud, addr);
        if (pmd_none(*pmd) || !pmd_present(*pmd) ||
            pmd_large(*pmd) || pmd_bad(*pmd))
            continue;

        pte = pte_offset_map(pmd, addr);

        if (!pte_present(*pte)) {
            pte_unmap(pte);
            continue;
        }

        pfn = pte_pfn(*pte);
        pa = (phys_addr_t)pfn << PAGE_SHIFT;

        pte_unmap(pte);

        pr_info("VA 0x%lx -> PFN 0x%lx (PA 0x%llx)\n",
                addr, pfn, (unsigned long long)pa);

        mapped_pages++;
    }

    pr_info("[memory_status] Mapped pages: %lu\n", mapped_pages);
}

static int __init memory_status_init(void)
{
    struct task_struct *task;
    struct mm_struct *mm;

    traverse_all_pages();

    if (pid < 0) {
        return 0;
    }

    task = pid_task(find_vpid(pid), PIDTYPE_PID);

    if (!task) {
        pr_err("[memory_status] PID %d not found\n", pid);
        return -ESRCH;
    }

    mm = get_task_mm(task);
    if (!mm) {
        pr_err("[memory_status] PID %d has no mm_struct (kernel thread?)\n", pid);
        return -EINVAL;
    }

    pr_info("[memory_status] ===== Target process: %s (pid=%d) =====\n",
            task->comm, pid);

    show_vmas(mm);
    traverse_page_table(mm);

    mmput(mm);
    return 0;
}

static void __exit memory_status_exit(void)
{
    pr_info("[memory_status] Module unloaded.\n");
}

module_init(memory_status_init);
module_exit(memory_status_exit);