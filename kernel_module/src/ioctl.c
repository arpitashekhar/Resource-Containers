//////////////////////////////////////////////////////////////////////
//                      North Carolina State University
//
//
//
//                             Copyright 2016
//
////////////////////////////////////////////////////////////////////////
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
// This program is distributed in the hope it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
//
////////////////////////////////////////////////////////////////////////
//
//   Author:  Hung-Wei Tseng, Yu-Chia Liu
//
//   Description:
//     Core of Kernel Module for Processor Container
//
////////////////////////////////////////////////////////////////////////
// Project 1 : Arpita Shekhar, ashekha; Bhavik Patel, bcpatel;
///////////////////////////////////////////////////////////////////////
#include "processor_container.h"
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <asm/current.h>
#include <linux/types.h>


// Struct for: List of tasks within a container
struct thread_node{
    int cid;
    int tid;
    struct task_struct *task;
    struct thread_node *tnext;
};
typedef struct thread_node t_node;

// Struct for: List of all the containers
struct container_node{
    int cid;
    t_node *t_head;
    t_node *t_current;
    struct container_node *cnext;
};
typedef struct container_node c_node;

// Head of container list
c_node *container_head = NULL;
static DEFINE_MUTEX(lock);

// Insert thread into container
t_node * insert_thread(int cid, t_node *t_head){
    t_node *new_node;
    new_node = kmalloc(sizeof(t_node),GFP_KERNEL);
    new_node->cid = cid;
    new_node->task = current;
    new_node->tid = current->pid;
    new_node->tnext = NULL;
    if(t_head == NULL){         // if no task is present within a container
        t_head = new_node;
    }else{                      // otherwise, append the current task at the end of task list
        t_node *current_node = t_head;
        while(current_node->tnext != NULL){
            current_node = current_node->tnext;
        }
        current_node->tnext = new_node;
        set_current_state(TASK_INTERRUPTIBLE);
        mutex_unlock(&lock);
        schedule();
        set_current_state(TASK_RUNNING);
        mutex_lock(&lock);
    }
    return new_node;
}

// Add container to the container list
void insert_container(__u64 cid){
   c_node *new_node;
   t_node *thread_node;
   new_node =  kmalloc(sizeof(c_node),GFP_KERNEL);
   new_node->cid = (int)cid;
   thread_node = insert_thread((int)cid, NULL);
   new_node->t_head = thread_node;
   new_node->t_current = thread_node;
   new_node->cnext = container_head;
   container_head = new_node;
}

// Check if a container is already present in container list
c_node * is_container_present(__u64 cid){
    c_node *current_node = container_head;
    while(current_node != NULL){
        if((int)cid == current_node->cid){
            return current_node;
        }
        current_node = current_node->cnext;
    }
    return current_node;
}

// Delete the container from the container list
void delete_container(int cid){
    c_node *prev_node;
    c_node *current_node = container_head;
    
    // deleting at head
    if(current_node != NULL && current_node->cid == cid){
        container_head = current_node->cnext;
        kfree(current_node);
        printk(KERN_INFO "Delete: Container deleted with ID:  %d", cid); 
        return;
    }

    // iterating to find the container to delete
    while(current_node != NULL && current_node->cid != cid){
        prev_node = current_node;
        current_node = current_node->cnext;
    }

    // container doesn't exist
    if(current_node == NULL){
         printk(KERN_INFO "Delete: Container does not exists with ID:  %d", cid);
         return;
    }

    prev_node->cnext = current_node->cnext;
    kfree(current_node);
    printk(KERN_INFO "Delete: Container deleted with ID:  %d", cid);
}

// Delete thread from the thread list, within a container
void delete_thread(c_node *current_container){
    struct task_struct *task = current;
    t_node *prev_node;
    t_node *current_node = current_container->t_head;
    
    printk(KERN_INFO "Delete: TID: %d, ContainerID: %d", task->pid, current_container->cid);

    // if container doesn't have any tasks; this condition should not happen!
    if(current_container->t_head == NULL){
        delete_container(current_container->cid);
        return;
    }

    // Deleting the head node
    if(current_node != NULL && current_node->tid == task->pid){
        current_container->t_head = current_node->tnext;
        current_container->t_current = current_node->tnext;
        kfree(current_node);
        printk(KERN_INFO "Delete: Thread deleted with ID:  %d", task->pid);
        
        // Deleting container if current task was last task in the container, otherwise wake up the next task
        if(current_container->t_head == NULL){
            delete_container(current_container->cid);
        }else{
            wake_up_process(current_container->t_head->task);
        }
        return;
    }

    // Deleting thread other than head
    while(current_node != NULL && current_node->tid != task->pid){
        prev_node = current_node;
        current_node = current_node->tnext;
    }

    if(current_node == NULL){
         printk(KERN_INFO "Delete: Thread does not exists with ID:  %d", task->pid);
         return;
    }

    prev_node->tnext = current_node->tnext;
    
    // Waking up next task
    if(prev_node->tnext != NULL){
        current_container->t_current = prev_node->tnext;
        wake_up_process(prev_node->tnext->task);    
    }else{
        // Waking up the head task when deleted the last task
        current_container->t_current = current_container->t_head;
        wake_up_process(current_container->t_head->task);
    }
    kfree(current_node);
    printk(KERN_INFO "Delete: Thread deleted with ID:  %d", task->pid);
}

/**
 * Delete the task in the container.
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), 
 */
int processor_container_delete(struct processor_container_cmd __user *user_cmd)
{
    mutex_lock(&lock);
     __u64 container_id;
    c_node *current_container;
    struct processor_container_cmd *container = kmalloc(sizeof(*user_cmd),GFP_KERNEL);

    copy_from_user (container, user_cmd, sizeof(*user_cmd));
    container_id = container->cid;

    // find the container and delete the task within it, if present
    current_container = is_container_present(container_id);
    if(current_container != NULL){
        delete_thread(current_container);
    }
    kfree(container);
    mutex_unlock(&lock);
    return 0;
}

/**
 * Create a task in the corresponding container.
 * external functions needed:
 * copy_from_user(), mutex_lock(), mutex_unlock(), set_current_state(), schedule()
 * 
 * external variables needed:
 * struct task_struct* current  
 */
int processor_container_create(struct processor_container_cmd __user *user_cmd)
{
    mutex_lock(&lock);
    __u64 container_id;
    c_node *current_container;
    struct processor_container_cmd *container = kmalloc(sizeof(*user_cmd),GFP_KERNEL);
    struct task_struct *task = current;

    copy_from_user (container, user_cmd, sizeof(*user_cmd));
    container_id = container->cid;
    printk(KERN_INFO "Create: TID: %d, ContainerID: %lld", task->pid, container_id);

    // if container exists, add thread otherwise create container and add thread
    current_container = is_container_present(container_id);
    if(current_container == NULL){
        insert_container(container_id);
    }else{
       insert_thread((int)container_id, current_container->t_head);
    }
    kfree(container);
    mutex_unlock(&lock);
    return 0;
}

/**
 * switch to the next task in the next container
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), set_current_state(), schedule()
 */
int processor_container_switch(struct processor_container_cmd __user *user_cmd)
{
    mutex_lock(&lock);

    t_node *current_thread;
    c_node *current_container = container_head;
    struct task_struct *current_task = current;
    printk(KERN_INFO "Switch: Started");

    // iterating through container list to find the container of current thread
    while(current_container != NULL){
        current_thread = current_container->t_current;
        if(current_thread == current_container->t_head){
            // When Head and Current thread of container are same
            if(current_thread->tnext != NULL && current_thread->tid == current_task->pid){
                printk(KERN_INFO "Switch: Current Running Task ID: %d, ContainerID: %d", current_task->pid, current_container->cid);
                set_current_state(TASK_INTERRUPTIBLE);
                current_container->t_current = current_thread->tnext;
                wake_up_process(current_thread->tnext->task);
                printk(KERN_INFO "Switch: Next Scheduled Task ID: %d, ContainerID: %d", current_thread->tnext->task->pid, current_container->cid);
                mutex_unlock(&lock);
                printk(KERN_INFO "Switch: Completed");
                schedule();
                set_current_state(TASK_RUNNING);
                return 0;
            }else if(current_thread->tid == current_task->pid){
                // Do nothing for only one thread in container
                printk(KERN_INFO "Switch: (Only one task in container) Current Running Task ID: %d, ContainerID: %d", current_task->pid, current_container->cid);
                mutex_unlock(&lock);
                printk(KERN_INFO "Switch: Completed");
                return 0;
            }
        } else {
            // When head and current threads are different
            if(current_thread->tid == current_task->pid){
                printk(KERN_INFO "Switch: Current Running Task ID: %d, ContainerID: %d", current_task->pid, current_container->cid);
                set_current_state(TASK_INTERRUPTIBLE);
                if(current_thread->tnext != NULL){
                    current_container->t_current = current_thread->tnext;
                    wake_up_process(current_thread->tnext->task);
                    printk(KERN_INFO "Switch: Next Scheduled Task ID: %d, ContainerID: %d", current_thread->tnext->task->pid, current_container->cid);
                }else{
                    current_container->t_current = current_container->t_head;
                    wake_up_process(current_container->t_head->task);
                    printk(KERN_INFO "Switch: Next Scheduled Task ID: %d, ContainerID: %d", current_container->t_head->task->pid, current_container->cid);
                }
                mutex_unlock(&lock);
                printk(KERN_INFO "Switch: Completed");
                schedule();
                set_current_state(TASK_RUNNING);
                return 0;
            }
        }
    
        // Check in next container        
        current_container = current_container->cnext;
    }
    printk(KERN_INFO "Switch: Could not find Task ID: %d, in any of the containers.", current_task->pid);
    mutex_unlock(&lock);
    return 0;
}

/**
 * control function that receive the command in user space and pass arguments to
 * corresponding functions.
 */
int processor_container_ioctl(struct file *filp, unsigned int cmd,
                              unsigned long arg)
{
    switch (cmd)
    {
    case PCONTAINER_IOCTL_CSWITCH:
        return processor_container_switch((void __user *)arg);
    case PCONTAINER_IOCTL_CREATE:
        return processor_container_create((void __user *)arg);
    case PCONTAINER_IOCTL_DELETE:
        return processor_container_delete((void __user *)arg);
    default:
        return -ENOTTY;
    }
}
