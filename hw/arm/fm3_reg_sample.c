#include "hw/sysbus.h"
#include "sysemu/char.h"
#include "qapi/qmp/qerror.h"
#include "hw/devices.h"
#include "hw/arm/arm.h"

#include <stdlib.h>
/*
 ***
  ���̃��W���[�����T�|�[�g����n�[�h�E�F�A���̒�`
  */

/* �n�[�h�E�F�A���� */
#define TYPE_FM3_REG_SAMPLE	"fm3.RegSample"
#define FM3_REG_SAMPLE(obj) \
    OBJECT_CHECK(Fm3RegState, (obj), TYPE_FM3_REG_SAMPLE)	/* obj��Fm3RegState�ŃL���X�g���Ă����v���m�F����B */

#define TYPE_FM3_CHRDEV_REG_SAMPLE	"fm3-ChrDev-RegSample"
#define FM3_CHRDEV_REG_SAMPLE(obj) \
    OBJECT_CHECK(Fm3ChrDevRegState, (obj), TYPE_FM3_CHRDEV_REG_SAMPLE)	/* obj��Fm3ChrDevRegState�ŃL���X�g���Ă����v���m�F����B */


/* �����I�ȃn�[�h�E�F�A���\���� */
typedef struct {
	/* �K�{���� */
	SysBusDevice parent_obj;
	MemoryRegion iomem;

	/* �n�[�h�E�F�A��` */
	qemu_irq	irq0;
	uint32_t	reg0;
	uint32_t	reg1;
} Fm3RegState;

typedef struct {
	/* �K�{���� */
	SysBusDevice busdev;
	CharDriverState *chr;

	/* �n�[�h�E�F�A��` */
	uint32_t	reg0;
	uint32_t	reg1;
} Fm3ChrDevRegState;
Fm3ChrDevRegState	*CheDevState;

/* ���������֐� */

/*
 -- ���[�h�������ɌĂ΂��֐� --
 *opaque	: memory_region_init_io�̑�4����
 offset		: �A�h���X
 size		: �A�N�Z�X�T�C�Y
 return		: �ǂݏo�����f�[�^
 */
static uint64_t fm3_reg_read(void *opaque, hwaddr offset,
                            unsigned size)
{
	Fm3RegState *s = (Fm3RegState *)opaque;
	printf("read offset = %x, size = %x\n", offset, size);
	uint64_t retval = 0;

	switch (offset) {
	case 0:
		retval = s->reg0;
		break;
	case 1:
		retval = s->reg1;
		break;
	default:
		retval = 0xCCCCCCCC;
		break;
	}
	return retval;
}

/*
 -- ���C�g�������ɌĂ΂��֐� --
 *opaque	: memory_region_init_io�̑�4����
 offset		: �A�h���X
 value		: �f�[�^
 size		: �A�N�Z�X�T�C�Y
 */
static void fm3_reg_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
	Fm3RegState *s = (Fm3RegState *)opaque;
	printf("write:offset = %x, value = %x, size = %x\n", offset, (unsigned long)value, size);
	switch (offset) {
	case 0:
		s->reg0 = value;
		break;
	case 4:
		s->reg1 = value;
		printf("setirq\n");
		qemu_set_irq(s->irq0, value);
		break;
	default:
		break;
	}
}

/*
 -- ChrDev����̓ǂݏo���\�����m�F����i�O������̏������݁j�֐� --
 *opaque	: memory_region_init_io�̑�4����
 */
static int fm3_chrdev_can_read(void *opaque)
{
    //Fm3GpioState *s = opaque;

    if (CheDevState)
        return 255;
    else
        return 0;
}

/*
 -- ChrDev����̓ǂݏo���i�O������̏������݁j�֐� --
 *opaque	: memory_region_init_io�̑�4����
 buf		: �f�[�^�o�b�t�@�̃|�C���^
 size		: �T�C�Y
 */
static void fm3_chrdev_read(void *opaque, const uint8_t *buf, int size)
{
    Fm3ChrDevRegState *s = opaque;
	
	char recv_string[256];
	int i;
	
	memset(recv_string, 0, 256);
	for (i = 0; i < size; i++) {
		recv_string[i] = *(buf + i);
		if (i > 253) {
			break;
		}
	}
	qemu_chr_fe_write(s->chr, (const uint8_t *)buf, size);
}

/*
 -- ChrDev�̃C�x���g�������ɌĂ΂��֐� --
 *opaque	: memory_region_init_io�̑�4����
 event		: �C�x���g
 */
static void fm3_chrdev_event(void *opaque, int event)
{
    //Fm3GpioControlState *s = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
    case CHR_EVENT_CLOSED:
        break;
    }
}


/* �����Œ�`���ꂽ�֐���QEMU��CPU����Ă΂��B */
static const MemoryRegionOps fm3_reg_mem_ops = {
    .read = fm3_reg_read,				/* ���[�h�������ɌĂ΂�� */
    .write = fm3_reg_write,				/* ���C�g�������ɌĂ΂�� */
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* ���Z�b�g�֐� */
static void fm3_reg_reset(DeviceState *d)
{
    Fm3RegState *s = FM3_REG_SAMPLE(d);
	s->reg0 = 0;
	s->reg1 = 0;
}

static void fm3_chrdev_reg_reset(DeviceState *d)
{
    Fm3ChrDevRegState *s = FM3_CHRDEV_REG_SAMPLE(d);
	s->reg0 = 0;
	s->reg1 = 0;
}

/* �������B�n�[�h�E�F�A���������ꂽ�Ƃ��ɌĂ΂��B */
static int fm3_reg_init(SysBusDevice *dev)
{
	DeviceState	*devs	= DEVICE(dev);
    Fm3RegState	*s		= FM3_REG_SAMPLE(devs);

	sysbus_init_irq(dev, &s->irq0);

    memory_region_init_io(&s->iomem, OBJECT(s), &fm3_reg_mem_ops, s, TYPE_FM3_REG_SAMPLE, 0x10);		// ���W�X�^��2�����Ȃ����A�K����0x10�o�C�g�ɂ���
    sysbus_init_mmio(dev, &s->iomem);

    return 0;
}

static int fm3_chrdev_reg_init(SysBusDevice *dev)
{
    Fm3ChrDevRegState	*s	= FM3_CHRDEV_REG_SAMPLE(dev);
	if (s->chr == NULL) {
		qerror_report(QERR_MISSING_PARAMETER, "chardev");
		return -1;
	}
    qemu_chr_add_handlers(s->chr, fm3_chrdev_can_read,
                          fm3_chrdev_read, fm3_chrdev_event, s);
	CheDevState = s;

    return 0;
}


/* ������� */
static Property fm3_reg_properties[] = {
    DEFINE_PROP_UINT32("reg0"			, Fm3RegState	, reg0			, 0),
    DEFINE_PROP_UINT32("reg1"			, Fm3RegState	, reg1			, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static Property fm3_chrdev_reg_properties[] = {
    DEFINE_PROP_CHR("chardev"			, Fm3ChrDevRegState	, chr),
    DEFINE_PROP_UINT32("reg0"			, Fm3ChrDevRegState	, reg0			, 0),
    DEFINE_PROP_UINT32("reg1"			, Fm3ChrDevRegState	, reg1			, 0),
    DEFINE_PROP_END_OF_LIST(),
};


/* �������B�n�[�h�E�F�A�o�^����QEMU����Ă΂��B */
static void fm3_reg_class_init(ObjectClass *klass, void *data)
{
	DeviceClass			*dc	= DEVICE_CLASS(klass);
	SysBusDeviceClass	*k	= SYS_BUS_DEVICE_CLASS(klass);

	k->init		= fm3_reg_init;			/* �������֐���o�^		*/
	dc->desc	= TYPE_FM3_REG_SAMPLE;	/* �n�[�h�E�F�A����		*/
	dc->reset	= fm3_reg_reset;		/* ���Z�b�g���ɌĂ΂�� */
	dc->props	= fm3_reg_properties;	/* �������				*/
}

static void fm3_chrdev_reg_class_init(ObjectClass *klass, void *data)
{
	DeviceClass			*dc	= DEVICE_CLASS(klass);
	SysBusDeviceClass	*k	= SYS_BUS_DEVICE_CLASS(klass);

	k->init		= fm3_chrdev_reg_init;			/* �������֐���o�^		*/
	dc->desc	= TYPE_FM3_CHRDEV_REG_SAMPLE;	/* �n�[�h�E�F�A����		*/
	dc->reset	= fm3_chrdev_reg_reset;		/* ���Z�b�g���ɌĂ΂�� */
	dc->props	= fm3_chrdev_reg_properties;	/* �������				*/
	dc->cannot_instantiate_with_device_add_yet = false;
}

/* �n�[�h�E�F�A���B */
static const TypeInfo fm3_reg_info = {
	.name			= TYPE_FM3_REG_SAMPLE,	/* �n�[�h�E�F�A����		*/
	.parent			= TYPE_SYS_BUS_DEVICE,	/* �ڑ��o�X				*/
	.instance_size	= sizeof(Fm3RegState),	/* �C���X�^���X�T�C�Y	*/
	.class_init		= fm3_reg_class_init,	/* �������֐��̃|�C���^	*/
};

static const TypeInfo fm3_chrdev_reg_info = {
	.name			= TYPE_FM3_CHRDEV_REG_SAMPLE,	/* �n�[�h�E�F�A����		*/
	.parent			= TYPE_SYS_BUS_DEVICE,	/* �ڑ��o�X				*/
	.instance_size	= sizeof(Fm3ChrDevRegState),	/* �C���X�^���X�T�C�Y	*/
	.class_init		= fm3_chrdev_reg_class_init,	/* �������֐��̃|�C���^	*/
};

/* �n�[�h�E�F�A����QEMU�ɓo�^�B */
static void fm3_register_devices(void)
{
    type_register_static(&fm3_reg_info);
    type_register_static(&fm3_chrdev_reg_info);
}

type_init(fm3_register_devices)
