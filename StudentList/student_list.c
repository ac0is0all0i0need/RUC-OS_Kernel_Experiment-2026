#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/ioctl.h>
#include <linux/hashtable.h>
#include <linux/kfifo.h>
#include <linux/idr.h>
#include <linux/rbtree.h>

/* ---------------- 学生结构与链表/哈希 ---------------- */
struct student {
    int id;
    char name[16];
    int score;
    int idr_id;
    struct list_head list;
    struct hlist_node hnode_grade;
    struct hlist_node hnode_college;
    struct rb_node score_node;
};

/* ---------------- 学生成绩缓存 ---------------- */
struct student_score {
    int idr_id; // IDR 映射后的 student_id
    int score;
};

LIST_HEAD(student_list);

#define GRADE_BITS   8
#define COLLEGE_BITS 8
#define GRADE_BUCKETS   (1 << GRADE_BITS)
#define COLLEGE_BUCKETS (1 << COLLEGE_BITS)
#define SCORE_FIFO_SIZE 32

static struct hlist_head grade_table[GRADE_BUCKETS];
static struct hlist_head college_table[COLLEGE_BUCKETS];
static struct idr student_idr;
static DECLARE_KFIFO(score_fifo, struct student_score, SCORE_FIFO_SIZE);
static struct rb_root score_root = RB_ROOT;

static inline int get_grade(int id) { return id / 1000000; }
static inline int get_college(int id) { return (id / 1000) % 1000; }
static inline unsigned int grade_hashfn(int grade) { return grade & (GRADE_BUCKETS - 1); }
static inline unsigned int college_hashfn(int college) { return college & (COLLEGE_BUCKETS - 1); }

static void init(void)
{
    /* TODO: initialize hash tables, IDR, KFIFO, and rbtree */
    int i;
    for(i = 0; i < GRADE_BUCKETS; i++)
        INIT_HLIST_HEAD(&grade_table[i]);
    for(i = 0; i < COLLEGE_BUCKETS; i++)
        INIT_HLIST_HEAD(&college_table[i]);
    idr_init(&student_idr);
    INIT_KFIFO(score_fifo);
    score_root = RB_ROOT;
}

static struct student *create_student(int id, const char *name)
{
    struct student *stu;
    stu = kmalloc(sizeof(struct student), GFP_KERNEL);
    if (!stu)
        return NULL;

    stu->id = id;
    strscpy(stu->name, name, sizeof(stu->name));
    stu->score = -1;
    stu->idr_id = -1;

    INIT_LIST_HEAD(&stu->list);
    INIT_HLIST_NODE(&stu->hnode_grade);
    INIT_HLIST_NODE(&stu->hnode_college);
    RB_CLEAR_NODE(&stu->score_node);

    return stu;
}
static struct student *find_by_id(int id);
static void add_student(struct student *stu)
{
    /* TODO: allocate an IDR ID and add the student to all indexes */
    int idr_id = idr_alloc(&student_idr, stu, 0, 0, GFP_KERNEL);
    if (idr_id < 0) {
        printk("Failed to allocate IDR ID for student %d\n", stu->id);
        kfree(stu);
        return;
    }
    if (find_by_id(stu->id)) {
        printk("Student with ID %d already exists\n", stu->id);
        idr_remove(&student_idr, idr_id);
        kfree(stu);
        return;
    }
    stu->idr_id = idr_id;

    list_add_tail(&stu->list, &student_list);
    hlist_add_head(&stu->hnode_grade, &grade_table[grade_hashfn(get_grade(stu->id))]);
    hlist_add_head(&stu->hnode_college, &college_table[college_hashfn(get_college(stu->id))]);
}
static void remove_score_node(struct student *stu);
static void dequeue_score(void);
static void del_student(struct student *stu)
{
    /* TODO: remove the student from all indexes and free it */
    list_del(&stu->list);
    hlist_del(&stu->hnode_grade);
    hlist_del(&stu->hnode_college);
    if (stu->idr_id >= 0)
        idr_remove(&student_idr, stu->idr_id);
    if (!RB_EMPTY_NODE(&stu->score_node))
    remove_score_node(stu);
    dequeue_score();
    kfree(stu); 
}

static struct student *find_by_id(int id)
{
    /* TODO: find a student by student ID */
    struct student *stu;
    list_for_each_entry(stu, &student_list, list) {
        if (stu->id == id)
            return stu;
    }
    return NULL;
}

static struct student *find_by_idr_id(int idr_id)
{
    /* TODO: find a student by IDR ID */
    struct student *stu = idr_find(&student_idr, idr_id);
    return stu;
}

static void enqueue_score(const struct student_score *score)
{
    /* TODO: validate and enqueue a score record to KFIFO */
    if (!score)
        return;
    if (kfifo_is_full(&score_fifo)) {
        printk("Score FIFO is full, dropping score for IDR %d\n", score->idr_id);
        return;
    }
    if (score->score < 0 || score->score > 100)
    return;
    if (score->idr_id < 0)
    return;
    kfifo_in(&score_fifo, score, 1);
}
static void insert_score_node(struct student *stu)
{
    /* TODO: insert the student into the score rbtree */
    struct rb_node **new = &score_root.rb_node;
    struct rb_node *parent = NULL;

    while (*new) {
        struct student *this =
            rb_entry(*new, struct student, score_node);

        parent = *new;

        if (stu->score < this->score)
            new = &((*new)->rb_left);
        else if (stu->score > this->score)
            new = &((*new)->rb_right);
        else if (stu->idr_id < this->idr_id)
            new = &((*new)->rb_left);
        else
            new = &((*new)->rb_right);
    }

    rb_link_node(&stu->score_node, parent, new);
    rb_insert_color(&stu->score_node, &score_root);
}

static void remove_score_node(struct student *stu)
{
    /* TODO: remove the student from the score rbtree */
     if (RB_EMPTY_NODE(&stu->score_node))
        return;

    rb_erase(&stu->score_node, &score_root);
    RB_CLEAR_NODE(&stu->score_node);
}

static void update_student_score(struct student *stu, int score)
{
    /* TODO: update the score and maintain the rbtree */
    if (!RB_EMPTY_NODE(&stu->score_node))
        remove_score_node(stu);

    stu->score = score;

    if (score >= 0)
        insert_score_node(stu);
}

static void dequeue_score(void)
{
    /* TODO: process all queued score events in FIFO order */
    struct student_score score_event;
    while (kfifo_out(&score_fifo, &score_event, 1) == 1) {
        struct student *stu = find_by_idr_id(score_event.idr_id);
        if (!stu) {
            printk("Student with IDR %d not found for score update\n", score_event.idr_id);
            continue;
        }
        update_student_score(stu, score_event.score);
        //update_student_score(stu, score_event.score);
    }
}




static struct student *find_top_student(void)
{
    /* TODO: return the student with the highest score */
    struct rb_node *node = score_root.rb_node;
    struct student *stu;
    if (!node) return NULL;
    while (node->rb_right)
        node = node->rb_right;
    stu = rb_entry(node, struct student, score_node);
    return stu;
}

/* ---------------- IOCTL 定义 ---------------- */

#define STUDENT_MAGIC   'S'

#define STUDENT_ADD             _IOW(STUDENT_MAGIC, 1, struct student_ioctl)
#define STUDENT_DEL             _IOW(STUDENT_MAGIC, 2, int)
#define STUDENT_QUERY_GRADE     _IOWR(STUDENT_MAGIC, 4, int)
#define STUDENT_QUERY_COLLEGE   _IOWR(STUDENT_MAGIC, 5, int)
#define STUDENT_SUBMIT_SCORE    _IOW(STUDENT_MAGIC, 6, struct student_score_ioctl)
#define STUDENT_QUERY_TOP       _IOR(STUDENT_MAGIC, 7, struct student_score_ioctl)

struct student_ioctl {
    int id;
    char name[16];
};

struct student_score_ioctl {
    int id;
    int score;
};

/* ---------------- 字符设备实现 ---------------- */

static dev_t devno;
static struct cdev student_cdev;
static struct class *student_class;

static long student_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct student_ioctl udata;
    struct student *stu;

    switch (cmd) {
    case STUDENT_ADD:
        if (copy_from_user(&udata, (void __user *)arg, sizeof(udata)))
            return -EFAULT;
        stu = create_student(udata.id, udata.name);
        if (!stu) return -ENOMEM;
        add_student(stu);
        printk("Added student %d %s\n", udata.id, udata.name);
        break;

    case STUDENT_DEL:
        {
            int id;
            if (copy_from_user(&id, (void __user *)arg, sizeof(int)))
                return -EFAULT;
            stu = find_by_id(id);
            if (!stu) return -ENOENT;
            del_student(stu);
            printk("Deleted student %d\n", id);
        }
        break;

    case STUDENT_QUERY_GRADE:
        {
            int grade, bucket;
            if (copy_from_user(&grade, (void __user *)arg, sizeof(int)))
                return -EFAULT;
            bucket = grade_hashfn(grade);
            hlist_for_each_entry(stu, &grade_table[bucket], hnode_grade) {
                if (get_grade(stu->id) == grade)
                    printk("Grade %d: %d %s\n", grade, stu->id, stu->name);
            }
        }
        break;

    case STUDENT_QUERY_COLLEGE:
        {
            int college, bucket;
            if (copy_from_user(&college, (void __user *)arg, sizeof(int)))
                return -EFAULT;
            bucket = college_hashfn(college);
            hlist_for_each_entry(stu, &college_table[bucket], hnode_college) {
                if (get_college(stu->id) == college)
                    printk("College %d: %d %s\n", college, stu->id, stu->name);
            }
        }
        break;

    case STUDENT_SUBMIT_SCORE:
        {
            struct student_score_ioctl score_data;
            struct student_score score_event;

            if (copy_from_user(&score_data, (void __user *)arg, sizeof(score_data)))
                return -EFAULT;
            stu = find_by_id(score_data.id);
            if (!stu)
                return -ENOENT;
            score_event.idr_id = stu->idr_id;
            score_event.score = score_data.score;
            enqueue_score(&score_event);
            printk("Submitted score: %d %d\n", score_data.id, score_data.score);
        }
        break;

    case STUDENT_QUERY_TOP:
        {
            struct student_score_ioctl score_data;

            dequeue_score();
            stu = find_top_student();
            if (!stu)
                return -ENOENT;
            score_data.id = stu->id;
            score_data.score = stu->score;
            if (copy_to_user((void __user *)arg, &score_data, sizeof(score_data)))
                return -EFAULT;
            printk("Top student: %d, score=%d\n", stu->id, stu->score);
        }
        break;

    default:
        return -ENOTTY;
    }
    return 0;
}

static struct file_operations student_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = student_ioctl,
};

/* ---------------- 模块入口/出口 ---------------- */

static int __init student_init(void)
{
    int ret;
    init();

    ret = alloc_chrdev_region(&devno, 0, 1, "student");
    if (ret < 0) return ret;

    cdev_init(&student_cdev, &student_fops);
    ret = cdev_add(&student_cdev, devno, 1);
    if (ret < 0) goto err_cdev;

    student_class = class_create(THIS_MODULE, "student");
    if (IS_ERR(student_class)) {
        ret = PTR_ERR(student_class);
        goto err_class;
    }
    device_create(student_class, NULL, devno, NULL, "student");

    printk("student module loaded\n");
    return 0;

err_class:
    cdev_del(&student_cdev);
err_cdev:
    unregister_chrdev_region(devno, 1);
    return ret;
}

static void __exit student_exit(void)
{
    struct student *stu, *tmp;
    list_for_each_entry_safe(stu, tmp, &student_list, list)
        del_student(stu);

    device_destroy(student_class, devno);
    class_destroy(student_class);
    cdev_del(&student_cdev);
    unregister_chrdev_region(devno, 1);

    printk("student module unloaded\n");
}

module_init(student_init);
module_exit(student_exit);

MODULE_LICENSE("GPL");
