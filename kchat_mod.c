/*
 * kchat: kernel based chat
 *
 * This module implements a character device which allows chatting between
 * user-space programs via the read() and write() system calls.
 */

#include <linux/kernel.h>   /* it's the kernel yo */
#include <linux/module.h>   /* it's a module yo */
#include <linux/init.h>     /* for module_{init,exit} */
#include <linux/slab.h>     /* kmalloc */
#include <linux/fs.h>       /* file_operations, file, etc... */
#include <linux/list.h>     /* all the list stuff */
#include <linux/mutex.h>    /* struct mutex */
#include <linux/rwsem.h>    /* struct rw_semaphore */
#include <asm/uaccess.h>    /* for put_user */

#define KCHAT_VMAJOR 0
#define KCHAT_VMINOR 1

#define SUCCESS 0

#define DEVICE_NAME "kchat"
#define KCHAT_BUF 2048
#define DIST(a, b) ((a) < (b) ? (b) - (a) : KCHAT_BUF + (b) - (a))

static int kchat_open(struct inode *, struct file *);
static int kchat_release(struct inode *, struct file *);
static ssize_t kchat_read(struct file *, char *, size_t, loff_t *);
static ssize_t kchat_write(struct file *, const char *, size_t, loff_t *);

/*
 * Chat server exists per-inode.
 */
struct kchat_server {
	struct inode *inode;
	struct list_head server_list; /* CONTAINED IN this list */
	struct list_head client_list; /* CONTAINS this list */
	struct mutex client_list_lock; /* protects client_list */
	char buffer[KCHAT_BUF];
	struct rw_semaphore buffer_lock;
	int end;
};

/*
 * Chat client exists per-process, per-file. Has reference to server and its own
 * unique offset.
 */
struct kchat_client {
	struct file *filp;
	struct kchat_server *server;
	struct list_head client_list; /* CONTAINED IN this list */
	int offset;
};

/*
 * This is the global server list. One per inode.
 */
static LIST_HEAD(server_list);
DEFINE_MUTEX(server_list_lock);

/*
 * Major number of our character device.
 */
static int major;

/*
 * Our file operations. These are registered for our character device to handle
 * open, close, read, write calls to our special device files.
 */
static struct file_operations kchat_fops = {
	.read = kchat_read,
	.write = kchat_write,
	.open = kchat_open,
	.release = kchat_release,
	.owner = THIS_MODULE
};

/*
 * Create a chat server for an inode. This assumes one does not already exist.
 * server_list_lock MUST already be held at this point
 * MUST check for null return (ENOMEM)
 */
static struct kchat_server *create_server(struct inode *inode)
{
	struct kchat_server *srv;
	srv = kmalloc(sizeof(struct kchat_server), GFP_KERNEL);

	// Early return for NULL! Must be checked.
	if (srv == NULL) {
		return NULL;
	}

	srv->inode = inode;
	srv->end = 0;
	INIT_LIST_HEAD(&srv->server_list);
	INIT_LIST_HEAD(&srv->client_list);
	mutex_init(&srv->client_list_lock);
	init_rwsem(&srv->buffer_lock);
	list_add(&server_list, &srv->server_list);
	return srv;
}

/*
 * Get the chat server for an inode, creating it if it doesn't exist.
 * requires the server_list_lock to be held
 */
static struct kchat_server *get_server(struct inode *inode)
{
	struct kchat_server *srv;
	list_for_each_entry(srv, &server_list, server_list) {
		if (srv->inode == inode) {
			return srv;
		}
	}
	srv = create_server(inode);
	return srv;
}

/*
 * Free server if it has no clients.
 * server_list lock must be held, will also try to hold the client list lock
 */
static void check_free_server(struct kchat_server *srv)
{
	// for safety, always lock server, then client when you need both
	mutex_lock_interruptible(&srv->client_list_lock);

	if (list_empty(&srv->client_list)) {
		// remove us from the server list
		list_del(&srv->server_list);
		kfree(srv);
	} else {
		// only release the client list lock if there are still clients
		mutex_unlock(&srv->client_list_lock);
	}
}

/*
 * Return the offset of the client with the most unread data.
 * client_list_lock must be held for this server
 */
static int blocking_offset(struct kchat_server *srv)
{
	struct kchat_client *cnt;
	int maxunread = 0;
	int offset = srv->end;
	list_for_each_entry(cnt, &srv->client_list, client_list) {
		if (DIST(cnt->offset, srv->end) > maxunread) {
			maxunread = DIST(cnt->offset, srv->end);
			offset = cnt->offset;
		}
	}
	return offset;
}

/*
 * Create a new client for a server.
 */
static struct kchat_client *create_client(struct file *filp,
					  struct kchat_server *srv)
{
	struct kchat_client *cnt;
	cnt = kmalloc(sizeof(struct kchat_client), GFP_KERNEL);

	if (cnt == NULL)
		return NULL;

	cnt->filp = filp;
	cnt->server = srv;
	INIT_LIST_HEAD(&cnt->client_list);
	cnt->offset = srv->end; // prevent invalid data

	mutex_lock_interruptible(&srv->client_list_lock);
	list_add(&srv->client_list, &cnt->client_list);
	mutex_unlock(&srv->client_list_lock);
	return cnt;
}

/*
 * Get the chat client for a file.
 */
static struct kchat_client *get_client(struct file *filp)
{
	struct kchat_server *srv;
	struct kchat_client *cnt;
	srv = get_server(filp->f_inode);

	mutex_lock_interruptible(&srv->client_list_lock);
	list_for_each_entry(cnt, &srv->client_list, client_list){
		if (cnt->filp == filp) {
			mutex_unlock(&srv->client_list_lock);
			return cnt;
		}
	}
	// presumably this won't happen, but should check for it if it does
	mutex_unlock(&srv->client_list_lock);
	return NULL;
}

/*
 * FILE OPERATIONS
 */

/*
 * Open - will get/create a server, and create a client as well
 */
static int kchat_open(struct inode * inode, struct file *filp)
{
	struct kchat_server *srv;
	struct kchat_client *cnt;

	// Obtain server list lock so that everything happens atomically to the
	// server list.
	mutex_lock_interruptible(&server_list_lock);

	srv = get_server(inode);
	if (!srv)
		goto server_create_failed;

	cnt = create_client(filp, srv);
	if (!cnt)
		goto client_create_failed;

	mutex_unlock(&server_list_lock);
	return SUCCESS;

client_create_failed:
	// free the server if we were the only ones who wanted it
	check_free_server(srv);
server_create_failed:
	mutex_unlock(&server_list_lock);
	return -ENOMEM;
}

/*
 * Close - destroy a client, and maybe the server
 */
static int kchat_release(struct inode *inode, struct file *filp)
{
	struct kchat_client *cnt;

	cnt = get_client(filp);

	if (!cnt)
		return -ENOENT;

	mutex_lock_interruptible(&cnt->server->client_list_lock);
	list_del(&cnt->client_list);
	mutex_unlock(&cnt->server->client_list_lock);

	mutex_lock_interruptible(&server_list_lock);
	check_free_server(cnt->server);
	mutex_unlock(&server_list_lock);

	kfree(cnt);
	return SUCCESS;
}

/*
 * Read - read from the server
 * Should not require any locks, since the "active" region of the buffer is
 * guaranteed not to be modified.
 */
static ssize_t kchat_read(struct file *filp, char *usrbuf, size_t length,
			  loff_t *offset)
{
	struct kchat_server *srv;
	struct kchat_client *cnt;
	int bytes_read = 0;

	cnt = get_client(filp);
	if (!cnt)
		return -ENOENT;
	srv = cnt->server;

	down_read(&srv->buffer_lock);
	while (length && DIST(cnt->offset, srv->end) > 0) {
		put_user(srv->buffer[cnt->offset], usrbuf++);
		length--;
		cnt->offset = (cnt->offset + 1) % KCHAT_BUF;
		bytes_read++;
	}
	up_read(&srv->buffer_lock);
	return bytes_read;
}

/*
 * Return whether or not there is any data for this file to read.
 */
unsigned int kchat_poll(struct file *filp, struct poll_table_struct *unused)
{
	struct kchat_client *cnt;
	struct kchat_server *srv;
	cnt = get_client(filp);
	if (!cnt)
		return false;
	srv = cnt->server;
	/* guaranteed to be positive... */
	return (unsigned int) DIST(cnt->offset, srv->end);
}

ssize_t kchat_write(struct file *filp, const char *usrbuf, size_t amt,
		    loff_t *unused)
{
	struct kchat_client *cnt;
	struct kchat_server *srv;

	int maxidx;
	int bytes_written = 0;

	cnt = get_client(filp);
	if (!cnt)
		return -ENOENT;
	srv = cnt->server;

	// determine how far we can write before we encounter space a client
	// has not read yet
	mutex_lock_interruptible(&srv->client_list_lock);
	maxidx = blocking_offset(srv);
	mutex_unlock(&srv->client_list_lock);

	// copy as much data from user space into the buffer
  down_write(&srv->buffer_lock);
	while (srv->end != maxidx && amt > 0) {
		get_user(srv->buffer[srv->end], usrbuf++);
		srv->end = (srv->end + 1) % KCHAT_BUF;
		amt--;
		bytes_written++;
	}
	up_write(&srv->buffer_lock);

	return bytes_written;
}

/*
 * Module initialization and exit routines.
 */
static int __init init_kchat(void)
{
	major = register_chrdev(0, DEVICE_NAME, &kchat_fops);
	if (major < 0) {
		printk(KERN_ALERT "Registering char device failed with %d\n",
		       major);
		return major;
	}

	printk(KERN_INFO "kchat v%d.%d -- assigned major number %d",
	       KCHAT_VMAJOR, KCHAT_VMINOR, major);
	printk(KERN_INFO "'mknod <filename> c %d 0' to make chat file!", major);
	return SUCCESS;
}

static void __exit exit_kchat(void)
{
	unregister_chrdev(major, DEVICE_NAME);
	if (!list_empty(&server_list)) {
		printk(KERN_ALERT "Uh-oh: kchat module unloaded without "
		       "all files being closed!");
	}
}

module_init(init_kchat);
module_exit(exit_kchat);

MODULE_AUTHOR("Stephen Brennan <stephen@brennan.io>");
MODULE_DESCRIPTION("Driver for simple chat between processes.");
MODULE_LICENSE("MIT");
