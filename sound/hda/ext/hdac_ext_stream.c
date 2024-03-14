// SPDX-License-Identifier: GPL-2.0-only
/*
 *  hdac-ext-stream.c - HD-audio extended stream operations.
 *
 *  Copyright (C) 2015 Intel Corp
 *  Author: Jeeja KP <jeeja.kp@intel.com>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <linux/delay.h>
#include <linux/slab.h>
#include <sound/pcm.h>
#include <sound/hda_register.h>
#include <sound/hdaudio_ext.h>
#include <sound/compress_driver.h>

/**
 * snd_hdac_ext_stream_init - initialize each stream (aka device)
 * @bus: HD-audio core bus
 * @hext_stream: HD-audio ext core stream object to initialize
 * @idx: stream index number
 * @direction: stream direction (SNDRV_PCM_STREAM_PLAYBACK or SNDRV_PCM_STREAM_CAPTURE)
 * @tag: the tag id to assign
 *
 * initialize the stream, if ppcap is enabled then init those and then
 * invoke hdac stream initialization routine
 */
static void snd_hdac_ext_stream_init(struct hdac_bus *bus,
				     struct hdac_ext_stream *hext_stream,
				     int idx, int direction, int tag)
{
	if (bus->ppcap) {
		hext_stream->pphc_addr = bus->ppcap + AZX_PPHC_BASE +
				AZX_PPHC_INTERVAL * idx;

		hext_stream->pplc_addr = bus->ppcap + AZX_PPLC_BASE +
				AZX_PPLC_MULTI * bus->num_streams +
				AZX_PPLC_INTERVAL * idx;
	}

	hext_stream->decoupled = false;
	snd_hdac_stream_init(bus, &hext_stream->hstream, idx, direction, tag);
}

/**
 * snd_hdac_ext_stream_init_all - create and initialize the stream objects
 *   for an extended hda bus
 * @bus: HD-audio core bus
 * @start_idx: start index for streams
 * @num_stream: number of streams to initialize
 * @dir: direction of streams
 */
int snd_hdac_ext_stream_init_all(struct hdac_bus *bus, int start_idx,
				 int num_stream, int dir)
{
	int stream_tag = 0;
	int i, tag, idx = start_idx;

	for (i = 0; i < num_stream; i++) {
		struct hdac_ext_stream *hext_stream =
				kzalloc(sizeof(*hext_stream), GFP_KERNEL);
		if (!hext_stream)
			return -ENOMEM;
		tag = ++stream_tag;
		snd_hdac_ext_stream_init(bus, hext_stream, idx, dir, tag);
		idx++;
	}

	return 0;

}
EXPORT_SYMBOL_GPL(snd_hdac_ext_stream_init_all);

/**
 * snd_hdac_ext_stream_free_all - free hdac extended stream objects
 *
 * @bus: HD-audio core bus
 */
void snd_hdac_ext_stream_free_all(struct hdac_bus *bus)
{
	struct hdac_stream *s, *_s;
	struct hdac_ext_stream *hext_stream;

	list_for_each_entry_safe(s, _s, &bus->stream_list, list) {
		hext_stream = stream_to_hdac_ext_stream(s);
		snd_hdac_ext_stream_decouple(bus, hext_stream, false);
		list_del(&s->list);
		kfree(hext_stream);
	}
}
EXPORT_SYMBOL_GPL(snd_hdac_ext_stream_free_all);

void snd_hdac_ext_stream_decouple_locked(struct hdac_bus *bus,
					 struct hdac_ext_stream *hext_stream,
					 bool decouple)
{
	struct hdac_stream *hstream = &hext_stream->hstream;
	u32 val;
	int mask = AZX_PPCTL_PROCEN(hstream->index);

	val = readw(bus->ppcap + AZX_REG_PP_PPCTL) & mask;

	if (decouple && !val)
		snd_hdac_updatel(bus->ppcap, AZX_REG_PP_PPCTL, mask, mask);
	else if (!decouple && val)
		snd_hdac_updatel(bus->ppcap, AZX_REG_PP_PPCTL, mask, 0);

	hext_stream->decoupled = decouple;
}
EXPORT_SYMBOL_GPL(snd_hdac_ext_stream_decouple_locked);

/**
 * snd_hdac_ext_stream_decouple - decouple the hdac stream
 * @bus: HD-audio core bus
 * @hext_stream: HD-audio ext core stream object to initialize
 * @decouple: flag to decouple
 */
void snd_hdac_ext_stream_decouple(struct hdac_bus *bus,
				  struct hdac_ext_stream *hext_stream, bool decouple)
{
	spin_lock_irq(&bus->reg_lock);
	snd_hdac_ext_stream_decouple_locked(bus, hext_stream, decouple);
	spin_unlock_irq(&bus->reg_lock);
}
EXPORT_SYMBOL_GPL(snd_hdac_ext_stream_decouple);

/**
 * snd_hdac_ext_stream_start - start a stream
 * @hext_stream: HD-audio ext core stream to start
 */
void snd_hdac_ext_stream_start(struct hdac_ext_stream *hext_stream)
{
	snd_hdac_updatel(hext_stream->pplc_addr, AZX_REG_PPLCCTL,
			 AZX_PPLCCTL_RUN, AZX_PPLCCTL_RUN);
}
EXPORT_SYMBOL_GPL(snd_hdac_ext_stream_start);

/**
 * snd_hdac_ext_stream_clear - stop a stream DMA
 * @hext_stream: HD-audio ext core stream to stop
 */
void snd_hdac_ext_stream_clear(struct hdac_ext_stream *hext_stream)
{
	snd_hdac_updatel(hext_stream->pplc_addr, AZX_REG_PPLCCTL, AZX_PPLCCTL_RUN, 0);
}
EXPORT_SYMBOL_GPL(snd_hdac_ext_stream_clear);

/**
 * snd_hdac_ext_stream_reset - reset a stream
 * @hext_stream: HD-audio ext core stream to reset
 */
void snd_hdac_ext_stream_reset(struct hdac_ext_stream *hext_stream)
{
	unsigned char val;
	int timeout;

	snd_hdac_ext_stream_clear(hext_stream);

	snd_hdac_updatel(hext_stream->pplc_addr, AZX_REG_PPLCCTL,
			 AZX_PPLCCTL_STRST, AZX_PPLCCTL_STRST);
	udelay(3);
	timeout = 50;
	do {
		val = readl(hext_stream->pplc_addr + AZX_REG_PPLCCTL) &
				AZX_PPLCCTL_STRST;
		if (val)
			break;
		udelay(3);
	} while (--timeout);
	val &= ~AZX_PPLCCTL_STRST;
	writel(val, hext_stream->pplc_addr + AZX_REG_PPLCCTL);
	udelay(3);

	timeout = 50;
	/* waiting for hardware to report that the stream is out of reset */
	do {
		val = readl(hext_stream->pplc_addr + AZX_REG_PPLCCTL) & AZX_PPLCCTL_STRST;
		if (!val)
			break;
		udelay(3);
	} while (--timeout);

}
EXPORT_SYMBOL_GPL(snd_hdac_ext_stream_reset);

/**
 * snd_hdac_ext_stream_setup -  set up the SD for streaming
 * @hext_stream: HD-audio ext core stream to set up
 * @fmt: stream format
 */
int snd_hdac_ext_stream_setup(struct hdac_ext_stream *hext_stream, int fmt)
{
	struct hdac_stream *hstream = &hext_stream->hstream;
	unsigned int val;

	/* make sure the run bit is zero for SD */
	snd_hdac_ext_stream_clear(hext_stream);
	/* program the stream_tag */
	val = readl(hext_stream->pplc_addr + AZX_REG_PPLCCTL);
	val = (val & ~AZX_PPLCCTL_STRM_MASK) |
		(hstream->stream_tag << AZX_PPLCCTL_STRM_SHIFT);
	writel(val, hext_stream->pplc_addr + AZX_REG_PPLCCTL);

	/* program the stream format */
	writew(fmt, hext_stream->pplc_addr + AZX_REG_PPLCFMT);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_hdac_ext_stream_setup);

static struct hdac_ext_stream *
hdac_ext_link_dma_stream_assign(struct hdac_bus *bus,
				struct snd_pcm_substream *substream)
{
	struct hdac_ext_stream *res = NULL;
	struct hdac_stream *hstream = NULL;

	if (!bus->ppcap) {
		dev_err(bus->dev, "stream type not supported\n");
		return NULL;
	}

	spin_lock_irq(&bus->reg_lock);
	list_for_each_entry(hstream, &bus->stream_list, list) {
		struct hdac_ext_stream *hext_stream = container_of(hstream,
								 struct hdac_ext_stream,
								 hstream);
		if (hstream->direction != substream->stream)
			continue;

		/* check if link stream is available */
		if (!hext_stream->link_locked) {
			res = hext_stream;
			break;
		}

	}
	if (res) {
		snd_hdac_ext_stream_decouple_locked(bus, res, true);
		res->link_locked = 1;
		res->link_substream = substream;
	}
	spin_unlock_irq(&bus->reg_lock);
	return res;
}

static struct hdac_ext_stream *
hdac_ext_host_dma_stream_assign(struct hdac_bus *bus,
				struct snd_pcm_substream *substream)
{
	struct hdac_ext_stream *res = NULL;
	struct hdac_stream *hstream = NULL;

	if (!bus->ppcap) {
		dev_err(bus->dev, "stream type not supported\n");
		return NULL;
	}

	spin_lock_irq(&bus->reg_lock);
	list_for_each_entry(hstream, &bus->stream_list, list) {
		struct hdac_ext_stream *hext_stream = container_of(hstream,
								 struct hdac_ext_stream,
								 hstream);
		if (hstream->direction != substream->stream)
			continue;

		if (!hstream->opened) {
			res = hext_stream;
			break;
		}
	}
	if (res) {
		snd_hdac_ext_stream_decouple_locked(bus, res, true);
		res->hstream.opened = 1;
		res->hstream.running = 0;
		res->hstream.substream = substream;
	}
	spin_unlock_irq(&bus->reg_lock);

	return res;
}

/**
 * snd_hdac_ext_stream_assign - assign a stream for the PCM
 * @bus: HD-audio core bus
 * @substream: PCM substream to assign
 * @type: type of stream (coupled, host or link stream)
 *
 * This assigns the stream based on the type (coupled/host/link), for the
 * given PCM substream, assigns it and returns the stream object
 *
 * coupled: Looks for an unused stream
 * host: Looks for an unused decoupled host stream
 * link: Looks for an unused decoupled link stream
 *
 * If no stream is free, returns NULL. The function tries to keep using
 * the same stream object when it's used beforehand.  when a stream is
 * decoupled, it becomes a host stream and link stream.
 */
struct hdac_ext_stream *snd_hdac_ext_stream_assign(struct hdac_bus *bus,
					   struct snd_pcm_substream *substream,
					   int type)
{
	struct hdac_ext_stream *hext_stream = NULL;
	struct hdac_stream *hstream = NULL;

	switch (type) {
	case HDAC_EXT_STREAM_TYPE_COUPLED:
		hstream = snd_hdac_stream_assign(bus, substream);
		if (hstream)
			hext_stream = container_of(hstream,
						   struct hdac_ext_stream,
						   hstream);
		return hext_stream;

	case HDAC_EXT_STREAM_TYPE_HOST:
		return hdac_ext_host_dma_stream_assign(bus, substream);

	case HDAC_EXT_STREAM_TYPE_LINK:
		return hdac_ext_link_dma_stream_assign(bus, substream);

	default:
		return NULL;
	}
}
EXPORT_SYMBOL_GPL(snd_hdac_ext_stream_assign);

/**
 * snd_hdac_ext_stream_release - release the assigned stream
 * @hext_stream: HD-audio ext core stream to release
 * @type: type of stream (coupled, host or link stream)
 *
 * Release the stream that has been assigned by snd_hdac_ext_stream_assign().
 */
void snd_hdac_ext_stream_release(struct hdac_ext_stream *hext_stream, int type)
{
	struct hdac_bus *bus = hext_stream->hstream.bus;

	switch (type) {
	case HDAC_EXT_STREAM_TYPE_COUPLED:
		snd_hdac_stream_release(&hext_stream->hstream);
		break;

	case HDAC_EXT_STREAM_TYPE_HOST:
		spin_lock_irq(&bus->reg_lock);
		/* couple link only if not in use */
		if (!hext_stream->link_locked)
			snd_hdac_ext_stream_decouple_locked(bus, hext_stream, false);
		snd_hdac_stream_release_locked(&hext_stream->hstream);
		spin_unlock_irq(&bus->reg_lock);
		break;

	case HDAC_EXT_STREAM_TYPE_LINK:
		spin_lock_irq(&bus->reg_lock);
		/* couple host only if not in use */
		if (!hext_stream->hstream.opened)
			snd_hdac_ext_stream_decouple_locked(bus, hext_stream, false);
		hext_stream->link_locked = 0;
		hext_stream->link_substream = NULL;
		spin_unlock_irq(&bus->reg_lock);
		break;

	default:
		dev_dbg(bus->dev, "Invalid type %d\n", type);
	}

}
EXPORT_SYMBOL_GPL(snd_hdac_ext_stream_release);

/**
 * snd_hdac_ext_cstream_assign - assign a host stream for compress
 * @bus: HD-audio core bus
 * @cstream: Compress stream to assign
 *
 * Assign an unused host stream for the given compress stream.
 * If no stream is free, NULL is returned. Stream is decoupled
 * before assignment.
 */
struct hdac_ext_stream *snd_hdac_ext_cstream_assign(struct hdac_bus *bus,
						    struct snd_compr_stream *cstream)
{
	struct hdac_ext_stream *res = NULL;
	struct hdac_stream *hstream;

	spin_lock_irq(&bus->reg_lock);
	list_for_each_entry(hstream, &bus->stream_list, list) {
		struct hdac_ext_stream *hext_stream = stream_to_hdac_ext_stream(hstream);

		if (hstream->direction != cstream->direction)
			continue;

		if (!hstream->opened) {
			res = hext_stream;
			break;
		}
	}

	if (res) {
		snd_hdac_ext_stream_decouple_locked(bus, res, true);
		res->hstream.opened = 1;
		res->hstream.running = 0;
		res->hstream.cstream = cstream;
	}
	spin_unlock_irq(&bus->reg_lock);

	return res;
}
EXPORT_SYMBOL_GPL(snd_hdac_ext_cstream_assign);
