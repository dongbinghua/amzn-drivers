// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright 2015-2022 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include "ena_phc.h"

#ifdef ENA_PHC_SUPPORT

static int ena_phc_adjfreq(struct ptp_clock_info *clock_info, s32 ppb)
{
	return -EOPNOTSUPP;
}

static int ena_phc_adjtime(struct ptp_clock_info *clock_info, s64 delta)
{
	return -EOPNOTSUPP;
}

static int ena_phc_enable(struct ptp_clock_info *clock_info, struct ptp_clock_request *rq, int on)
{
	return -EOPNOTSUPP;
}

#ifdef ENA_PHC_SUPPORT_GETTIME64
#ifdef ENA_PHC_SUPPORT_GETTIME64_EXTENDED
static int ena_phc_gettimex64(struct ptp_clock_info *clock_info, struct timespec64 *ts,
			      struct ptp_system_timestamp *sts)
{
	struct ena_phc_info *phc_info = container_of(clock_info, struct ena_phc_info, clock_info);
	unsigned long flags;
	u64 timestamp_nsec;
	int rc;

	spin_lock_irqsave(&phc_info->lock, flags);

	ptp_read_system_prets(sts);

	rc = ena_com_phc_get(phc_info->adapter->ena_dev, &timestamp_nsec);

	ptp_read_system_postts(sts);

	spin_unlock_irqrestore(&phc_info->lock, flags);

	*ts = ns_to_timespec64(timestamp_nsec);

	return rc;
}

#else /* ENA_PHC_SUPPORT_GETTIME64_EXTENDED */
static int ena_phc_gettime64(struct ptp_clock_info *clock_info, struct timespec64 *ts)
{
	struct ena_phc_info *phc_info = container_of(clock_info, struct ena_phc_info, clock_info);
	unsigned long flags;
	u64 timestamp_nsec;
	int rc;

	spin_lock_irqsave(&phc_info->lock, flags);

	rc = ena_com_phc_get(phc_info->adapter->ena_dev, &timestamp_nsec);

	spin_unlock_irqrestore(&phc_info->lock, flags);

	*ts = ns_to_timespec64(timestamp_nsec);

	return rc;
}

#endif /* ENA_PHC_SUPPORT_GETTIME64_EXTENDED */
static int ena_phc_settime64(struct ptp_clock_info *clock_info,
			     const struct timespec64 *ts)
{
	return -EOPNOTSUPP;
}

#else /* ENA_PHC_SUPPORT_GETTIME64 */
static int ena_phc_gettime(struct ptp_clock_info *clock_info, struct timespec *ts)
{
	struct ena_phc_info *phc_info = container_of(clock_info, struct ena_phc_info, clock_info);
	unsigned long flags;
	u64 timestamp_nsec;
	u32 remainder;
	int rc;

	spin_lock_irqsave(&phc_info->lock, flags);

	rc = ena_com_phc_get(phc_info->adapter->ena_dev, &timestamp_nsec);

	spin_unlock_irqrestore(&phc_info->lock, flags);

	ts->tv_sec = div_u64_rem(timestamp_nsec, NSEC_PER_SEC, &remainder);
	ts->tv_nsec = remainder;

	return rc;
}

static int ena_phc_settime(struct ptp_clock_info *clock_info, const struct timespec *ts)
{
	return -EOPNOTSUPP;
}

#endif /* ENA_PHC_SUPPORT_GETTIME64 */

static struct ptp_clock_info ena_ptp_clock_info = {
	.owner		= THIS_MODULE,
	.n_alarm	= 0,
	.n_ext_ts	= 0,
	.n_per_out	= 0,
	.pps		= 0,
	.adjfreq	= ena_phc_adjfreq,
	.adjtime	= ena_phc_adjtime,
#ifdef ENA_PHC_SUPPORT_GETTIME64
#ifdef ENA_PHC_SUPPORT_GETTIME64_EXTENDED
	.gettimex64	= ena_phc_gettimex64,
#else /* ENA_PHC_SUPPORT_GETTIME64_EXTENDED */
	.gettime64	= ena_phc_gettime64,
#endif /* ENA_PHC_SUPPORT_GETTIME64_EXTENDED */
	.settime64	= ena_phc_settime64,
#else /* ENA_PHC_SUPPORT_GETTIME64 */
	.gettime	= ena_phc_gettime,
	.settime	= ena_phc_settime,
#endif /* ENA_PHC_SUPPORT_GETTIME64 */
	.enable		= ena_phc_enable,
};

static int ena_phc_register(struct ena_adapter *adapter)
{
	struct pci_dev *pdev = adapter->pdev;
	struct ptp_clock_info *clock_info;
	struct ena_phc_info *phc_info;
	int rc = 0;

	phc_info = adapter->phc_info;
	clock_info = &phc_info->clock_info;

	phc_info->adapter = adapter;

	spin_lock_init(&phc_info->lock);

	/* Fill the ptp_clock_info struct and register PTP clock */
	*clock_info = ena_ptp_clock_info;
	snprintf(clock_info->name,
		 sizeof(clock_info->name),
		 "ena-ptp-%02x",
		 PCI_SLOT(pdev->devfn));

	phc_info->clock = ptp_clock_register(clock_info, &pdev->dev);
	if (IS_ERR(phc_info->clock)) {
		rc = PTR_ERR(phc_info->clock);
		netdev_err(adapter->netdev, "Failed registering ptp clock, error: %d\n", rc);
		phc_info->clock = NULL;
	}

	return rc;
}

bool ena_phc_enabled(struct ena_adapter *adapter)
{
	struct ena_phc_info *phc_info = adapter->phc_info;

	return (phc_info && phc_info->clock);
}

static void ena_phc_unregister(struct ena_adapter *adapter)
{
	struct ena_phc_info *phc_info = adapter->phc_info;

	if (ena_phc_enabled(adapter))
		ptp_clock_unregister(phc_info->clock);
}

int ena_phc_init(struct ena_adapter *adapter)
{
	struct ena_com_dev *ena_dev = adapter->ena_dev;
	struct net_device *netdev = adapter->netdev;
	int rc = -EOPNOTSUPP;

	/* Validate phc feature is supported in the device */
	if (!ena_com_phc_supported(ena_dev)) {
		netdev_dbg(netdev, "PHC feature is not supported\n");
		goto err_ena_com_phc_init;
	}

	/* Allocate and initialize device specific PHC info */
	rc = ena_com_phc_init(ena_dev);
	if (unlikely(rc)) {
		netdev_err(netdev, "Failed to init phc, error: %d\n", rc);
		goto err_ena_com_phc_init;
	}

	/* Configure PHC feature in driver and device */
	rc = ena_com_phc_config(ena_dev);
	if (unlikely(rc)) {
		netdev_err(netdev, "Failed to config phc, error: %d\n", rc);
		goto err_ena_com_phc_config;
	}

	/* Allocate and initialize driver specific PHC info */
	adapter->phc_info = vzalloc(sizeof(*adapter->phc_info));
	if (unlikely(!adapter->phc_info)) {
		rc = -ENOMEM;
		netdev_err(netdev, "Failed to alloc phc_info, error: %d\n", rc);
		goto err_ena_com_phc_config;
	}

	/* Register to PTP class driver */
	rc = ena_phc_register(adapter);
	if (unlikely(rc)) {
		netdev_err(netdev, "Failed to register phc, error: %d\n", rc);
		goto err_ena_phc_register;
	}

	return 0;

err_ena_phc_register:
	vfree(adapter->phc_info);
	adapter->phc_info = NULL;
err_ena_com_phc_config:
	ena_com_phc_destroy(ena_dev);
err_ena_com_phc_init:
	return rc;
}

void ena_phc_destroy(struct ena_adapter *adapter)
{
	ena_phc_unregister(adapter);

	if (likely(adapter->phc_info)) {
		vfree(adapter->phc_info);
		adapter->phc_info = NULL;
	}

	ena_com_phc_destroy(adapter->ena_dev);
}

int ena_phc_get_index(struct ena_adapter *adapter)
{
	struct ena_phc_info *phc_info = adapter->phc_info;

	if (ena_phc_enabled(adapter))
		return ptp_clock_index(phc_info->clock);

	return -1;
}

#endif /* ENA_PHC_SUPPORT */