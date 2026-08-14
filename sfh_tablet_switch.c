// SPDX-License-Identifier: GPL-2.0

#include <linux/amd-pmf-io.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#define DRIVER_NAME          "sfh_tablet_switch"
#define POLL_INTERVAL_MS     200
#define STABLE_READS_NEEDED  3

/*
 * Valores de platform_type proporcionados por AMD SFH:
 *
 * 0  unknown
 * 1  lid closed
 * 2  clamshell
 * 3  flat
 * 4  tent
 * 5  stand
 * 6  tablet
 * 7  book
 * 8  presentation
 * 9  pull-forward
 * 15 invalid
 */
enum sfh_platform_mode {
    SFH_MODE_UNKNOWN      = 0,
    SFH_MODE_LID_CLOSED   = 1,
    SFH_MODE_CLAMSHELL    = 2,
    SFH_MODE_FLAT         = 3,
    SFH_MODE_TENT         = 4,
    SFH_MODE_STAND        = 5,
    SFH_MODE_TABLET       = 6,
    SFH_MODE_BOOK         = 7,
    SFH_MODE_PRESENTATION = 8,
    SFH_MODE_PULL_FORWARD = 9,
    SFH_MODE_INVALID      = 0x0f,
};

static struct input_dev *tablet_input;
static struct delayed_work poll_work;

static u32 last_raw_mode = ~0U;
static int reported_state = -1;
static int candidate_state = -1;
static unsigned int candidate_reads;

/*
 * En este OmniBook hemos observado:
 *
 * 4 = tienda
 * 6 = tableta
 * 7 = libro
 *
 * El modo 5 se considera también postura convertible.
 */
static int sfh_mode_is_tablet(u32 mode)
{
    switch (mode) {
    case SFH_MODE_TENT:
    case SFH_MODE_STAND:
    case SFH_MODE_TABLET:
    case SFH_MODE_BOOK:
        return 1;

    case SFH_MODE_UNKNOWN:
    case SFH_MODE_LID_CLOSED:
    case SFH_MODE_CLAMSHELL:
    case SFH_MODE_FLAT:
        return 0;

    default:
        /*
         * No cambiar el estado ante modos no observados o inválidos.
         */
        return -1;
    }
}

static void report_tablet_state(int enabled, u32 mode, u32 placement)
{
    input_report_switch(
        tablet_input,
        SW_TABLET_MODE,
        enabled
    );
    input_sync(tablet_input);

    reported_state = enabled;

    pr_info(
        DRIVER_NAME
        ": SW_TABLET_MODE=%d platform_type=%u placement=%u\n",
        enabled,
        mode,
        placement
    );
}

static void poll_sfh_mode(struct work_struct *work)
{
    struct amd_sfh_info info = { 0 };
    int state;
    int ret;

    ret = amd_get_sfh_info(&info, MT_SRA);
    if (ret) {
        pr_warn_ratelimited(
            DRIVER_NAME ": amd_get_sfh_info error=%d\n",
            ret
        );
        goto reschedule;
    }

    if (info.platform_type != last_raw_mode) {
        pr_info(
            DRIVER_NAME ": raw platform_type=%u placement=%u\n",
            info.platform_type,
            info.laptop_placement
        );

        last_raw_mode = info.platform_type;
    }

    state = sfh_mode_is_tablet(info.platform_type);

    if (state < 0)
        goto reschedule;

    /*
     * Exige tres lecturas consecutivas iguales para evitar cambios
     * espurios mientras la bisagra atraviesa posiciones intermedias.
     */
    if (state != candidate_state) {
        candidate_state = state;
        candidate_reads = 1;
    } else if (candidate_reads < STABLE_READS_NEEDED) {
        candidate_reads++;
    }

    if (
        candidate_reads >= STABLE_READS_NEEDED &&
        state != reported_state
    ) {
        report_tablet_state(
            state,
            info.platform_type,
            info.laptop_placement
        );
    }

reschedule:
    schedule_delayed_work(
        &poll_work,
        msecs_to_jiffies(POLL_INTERVAL_MS)
    );
}

static int __init sfh_tablet_switch_init(void)
{
    int ret;

    tablet_input = input_allocate_device();
    if (!tablet_input)
        return -ENOMEM;

    tablet_input->name = "AMD SFH Tablet Mode Switch";
    tablet_input->phys = "amd-sfh/tablet-mode";
    tablet_input->id.bustype = BUS_HOST;
    tablet_input->id.vendor = 0x1022;
    tablet_input->id.product = 0x0001;
    tablet_input->id.version = 1;

    input_set_capability(
        tablet_input,
        EV_SW,
        SW_TABLET_MODE
    );

    ret = input_register_device(tablet_input);
    if (ret) {
        input_free_device(tablet_input);
        tablet_input = NULL;
        return ret;
    }

    INIT_DELAYED_WORK(&poll_work, poll_sfh_mode);
    schedule_delayed_work(&poll_work, 0);

    pr_info(DRIVER_NAME ": iniciado\n");

    return 0;
}

static void __exit sfh_tablet_switch_exit(void)
{
    cancel_delayed_work_sync(&poll_work);

    /*
     * Deja GNOME en modo portátil antes de eliminar el dispositivo.
     */
    if (tablet_input) {
        input_report_switch(
            tablet_input,
            SW_TABLET_MODE,
            0
        );
        input_sync(tablet_input);

        input_unregister_device(tablet_input);
        tablet_input = NULL;
    }

    pr_info(DRIVER_NAME ": detenido\n");
}

module_init(sfh_tablet_switch_init);
module_exit(sfh_tablet_switch_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("vinylo");
MODULE_DESCRIPTION("AMD SFH automatic tablet mode switch");
MODULE_SOFTDEP("pre: amd_sfh");
