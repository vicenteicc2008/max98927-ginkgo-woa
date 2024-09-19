#include "opengmaxcodec.h"
#include "max98512.h"

#define bool int

static ULONG GmaxDebugLevel = 100;
static ULONG GmaxDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

NTSTATUS
DriverEntry(
	__in PDRIVER_OBJECT  DriverObject,
	__in PUNICODE_STRING RegistryPath
)
{
	NTSTATUS               status = STATUS_SUCCESS;
	WDF_DRIVER_CONFIG      config;
	WDF_OBJECT_ATTRIBUTES  attributes;

	GmaxPrint(DEBUG_LEVEL_INFO, DBG_INIT,
		"Driver Entry\n");

	WDF_DRIVER_CONFIG_INIT(&config, GmaxEvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

	//
	// Create a framework driver object to represent our driver.
	//

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&attributes,
		&config,
		WDF_NO_HANDLE
	);

	if (!NT_SUCCESS(status))
	{
		GmaxPrint(DEBUG_LEVEL_ERROR, DBG_INIT,
			"WdfDriverCreate failed with status 0x%x\n", status);
	}

	return status;
}

typedef enum platform {
	PlatformNone,
	PlatformRyzen,
	PlatformAmberLake,
	PlatformTigerLake
} Platform;

static Platform GetPlatform() {
	int cpuinfo[4];
	__cpuidex(cpuinfo, 0, 0);

	int temp = cpuinfo[2];
	cpuinfo[2] = cpuinfo[3];
	cpuinfo[3] = temp;

	char vendorName[13];
	RtlZeroMemory(vendorName, 13);
	memcpy(vendorName, &cpuinfo[1], 12);

	__cpuidex(cpuinfo, 1, 0);

	UINT16 family = (cpuinfo[0] >> 8) & 0xF;
	UINT8 model = (cpuinfo[0] >> 4) & 0xF;
	UINT8 stepping = cpuinfo[0] & 0xF;
	if (family == 0xF || family == 0x6) {
		model += (((cpuinfo[0] >> 16) & 0xF) << 4);
	}
	if (family == 0xF) {
		family += (cpuinfo[0] >> 20) & 0xFF;
	}

	if (strcmp(vendorName, "AuthenticAMD") == 0) {
		return PlatformRyzen; //family 23 for Picasso / Dali
	}
	else if (strcmp(vendorName, "GenuineIntel") == 0) {
		if (model == 142)
			return PlatformAmberLake;
		else
			return PlatformTigerLake; //should be 140
	}
	return PlatformNone;
}

NTSTATUS gmax_reg_read(
	_In_ PGMAX_CONTEXT pDevice,
	uint16_t reg,
	uint8_t* data
) {
	uint8_t buf[2];
	buf[0] = (reg >> 8) & 0xff;
	buf[1] = reg & 0xff;

	uint8_t raw_data = 0;
	NTSTATUS status = SpbXferDataSynchronously(&pDevice->I2CContext, buf, sizeof(buf), &raw_data, sizeof(uint8_t));
	*data = raw_data;
	return status;
}

NTSTATUS gmax_reg_write(
	_In_ PGMAX_CONTEXT pDevice,
	uint16_t reg,
	uint8_t data
) {
	uint8_t buf[3];
	buf[0] = (reg >> 8) & 0xff;
	buf[1] = reg & 0xff;
	buf[2] = data;
	return SpbWriteDataSynchronously(&pDevice->I2CContext, buf, sizeof(buf));
}

NTSTATUS gmax_reg_update(
	_In_ PGMAX_CONTEXT pDevice,
	uint16_t reg,
	uint8_t mask,
	uint8_t val
) {
	uint8_t tmp = 0, orig = 0;

	NTSTATUS status = gmax_reg_read(pDevice, reg, &orig);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	tmp = orig & ~mask;
	tmp |= val & mask;

	if (tmp != orig) {
		status = gmax_reg_write(pDevice, reg, tmp);
	}
	return status;
}

struct initreg {
	UINT16 reg;
	UINT8 val;
};

struct initreg max98512_initregs[] = {
	{MAX98512_R0014_MEAS_ADC_THERM_WARN_THRESH, 0x75},
	{MAX98512_R0015_MEAS_ADC_THERM_SHDN_THRESH, 0x8C},
	{MAX98512_R0016_MEAS_ADC_THERM_HYSTERESIS, 0x8},
	{MAX98512_R0018_PCM_RX_EN_A, 0x3},
	{MAX98512_R0020_PCM_MODE_CFG, 0x58},
	{MAX98512_R0022_PCM_CLK_SETUP, 0x26},
	{MAX98512_R0023_PCM_SR_SETUP1, 0x8}
};

NTSTATUS toggleI2CAmp(
	_In_ PGMAX_CONTEXT pDevice,
	BOOLEAN enable
) {
	return gmax_reg_write(pDevice, pDevice->chipModel == 98512 ? MAX98512_R0038_AMP_EN, enable & 0x1);
}

NTSTATUS enableOutput(
	_In_ PGMAX_CONTEXT pDevice,
	BOOLEAN enable
) {
	NTSTATUS status;
	status = gmax_reg_write(pDevice, pDevice->chipModel == 98512 ? MAX98512_R0400_GLOBAL_SHDN, 1);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	LARGE_INTEGER Interval;
	Interval.QuadPart = -20;
	KeDelayExecutionThread(KernelMode, FALSE, &Interval);
	return toggleI2CAmp(pDevice, enable);
}



NTSTATUS
GetDeviceHID(
	_In_ WDFDEVICE FxDevice
)
{
	NTSTATUS status = STATUS_ACPI_NOT_INITIALIZED;
	ACPI_EVAL_INPUT_BUFFER_EX inputBuffer;
	RtlZeroMemory(&inputBuffer, sizeof(inputBuffer));

	inputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX;
	status = RtlStringCchPrintfA(
		inputBuffer.MethodName,
		sizeof(inputBuffer.MethodName),
		"_HID"
	);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	WDFMEMORY outputMemory;
	PACPI_EVAL_OUTPUT_BUFFER outputBuffer;
	size_t outputArgumentBufferSize = 32;
	size_t outputBufferSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) + outputArgumentBufferSize;

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = FxDevice;

	status = WdfMemoryCreate(&attributes,
		NonPagedPoolNx,
		0,
		outputBufferSize,
		&outputMemory,
		(PVOID*)&outputBuffer);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	RtlZeroMemory(outputBuffer, outputBufferSize);

	WDF_MEMORY_DESCRIPTOR inputMemDesc;
	WDF_MEMORY_DESCRIPTOR outputMemDesc;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputMemDesc, &inputBuffer, (ULONG)sizeof(inputBuffer));
	WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&outputMemDesc, outputMemory, NULL);

	status = WdfIoTargetSendInternalIoctlSynchronously(
		WdfDeviceGetIoTarget(FxDevice),
		NULL,
		IOCTL_ACPI_EVAL_METHOD_EX,
		&inputMemDesc,
		&outputMemDesc,
		NULL,
		NULL
	);
	if (!NT_SUCCESS(status)) {
		goto Exit;
	}

	if (outputBuffer->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE) {
		goto Exit;
	}

	if (outputBuffer->Count < 1) {
		goto Exit;
	}

	PGMAX_CONTEXT pDevice = GetDeviceContext(FxDevice);
	if (strncmp(outputBuffer->Argument[0].Data, "MX98512", outputBuffer->Argument[0].DataLength) == 0) {
		pDevice->chipModel = 98512;
	}
	else {
		status = STATUS_ACPI_INVALID_ARGUMENT;
	}

Exit:
	if (outputMemory != WDF_NO_HANDLE) {
		WdfObjectDelete(outputMemory);
	}
	return status;
}

NTSTATUS
GetDeviceUID(
	_In_ WDFDEVICE FxDevice,
	_In_ PINT32 PUID
)
{
	NTSTATUS status = STATUS_ACPI_NOT_INITIALIZED;
	ACPI_EVAL_INPUT_BUFFER_EX inputBuffer;
	RtlZeroMemory(&inputBuffer, sizeof(inputBuffer));

	inputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX;
	status = RtlStringCchPrintfA(
		inputBuffer.MethodName,
		sizeof(inputBuffer.MethodName),
		"_UID"
	);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	WDFMEMORY outputMemory;
	PACPI_EVAL_OUTPUT_BUFFER outputBuffer;
	size_t outputArgumentBufferSize = 32;
	size_t outputBufferSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) + outputArgumentBufferSize;

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = FxDevice;

	status = WdfMemoryCreate(&attributes,
		NonPagedPoolNx,
		0,
		outputBufferSize,
		&outputMemory,
		(PVOID*)&outputBuffer);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	RtlZeroMemory(outputBuffer, outputBufferSize);

	WDF_MEMORY_DESCRIPTOR inputMemDesc;
	WDF_MEMORY_DESCRIPTOR outputMemDesc;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputMemDesc, &inputBuffer, (ULONG)sizeof(inputBuffer));
	WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&outputMemDesc, outputMemory, NULL);

	status = WdfIoTargetSendInternalIoctlSynchronously(
		WdfDeviceGetIoTarget(FxDevice),
		NULL,
		IOCTL_ACPI_EVAL_METHOD_EX,
		&inputMemDesc,
		&outputMemDesc,
		NULL,
		NULL
	);
	if (!NT_SUCCESS(status)) {
		goto Exit;
	}

	if (outputBuffer->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE) {
		goto Exit;
	}

	if (outputBuffer->Count < 1) {
		goto Exit;
	}

	uint32_t uid;
	if (outputBuffer->Argument[0].DataLength >= 4) {
		uid = *(uint32_t*)outputBuffer->Argument->Data;
	}
	else if (outputBuffer->Argument[0].DataLength >= 2) {
		uid = *(uint16_t*)outputBuffer->Argument->Data;
	}
	else {
		uid = *(uint8_t*)outputBuffer->Argument->Data;
	}
	if (PUID) {
		*PUID = uid;
	}
	else {
		status = STATUS_ACPI_INVALID_ARGUMENT;
	}
Exit:
	if (outputMemory != WDF_NO_HANDLE) {
		WdfObjectDelete(outputMemory);
	}
	return status;
}
int CsAudioArg2 = 1;

static NTSTATUS GetIntegerProperty(
	_In_ WDFDEVICE FxDevice,
	char *propertyStr,
	UINT16 *property
) {
	PGMAX_CONTEXT pDevice = GetDeviceContext(FxDevice);
	WDFMEMORY outputMemory = WDF_NO_HANDLE;

	NTSTATUS status = STATUS_ACPI_NOT_INITIALIZED;

	size_t inputBufferLen = sizeof(ACPI_GET_DEVICE_SPECIFIC_DATA) + strlen(propertyStr) + 1;
	ACPI_GET_DEVICE_SPECIFIC_DATA* inputBuffer = ExAllocatePoolWithTag(NonPagedPool, inputBufferLen, GMAX_POOL_TAG);
	if (!inputBuffer) {
		goto Exit;
	}
	RtlZeroMemory(inputBuffer, inputBufferLen);

	inputBuffer->Signature = IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA_SIGNATURE;

	unsigned char uuidend[] = { 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01 };

	inputBuffer->Section.Data1 = 0xdaffd814;
	inputBuffer->Section.Data2 = 0x6eba;
	inputBuffer->Section.Data3 = 0x4d8c;
	memcpy(inputBuffer->Section.Data4, uuidend, sizeof(uuidend)); //Avoid Windows defender false positive

	strcpy(inputBuffer->PropertyName, propertyStr);
	inputBuffer->PropertyNameLength = strlen(propertyStr) + 1;

	PACPI_EVAL_OUTPUT_BUFFER outputBuffer;
	size_t outputArgumentBufferSize = 8;
	size_t outputBufferSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) + sizeof(ACPI_METHOD_ARGUMENT_V1) + outputArgumentBufferSize;

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = FxDevice;
	status = WdfMemoryCreate(&attributes,
		NonPagedPoolNx,
		0,
		outputBufferSize,
		&outputMemory,
		&outputBuffer);
	if (!NT_SUCCESS(status)) {
		goto Exit;
	}

	WDF_MEMORY_DESCRIPTOR inputMemDesc;
	WDF_MEMORY_DESCRIPTOR outputMemDesc;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputMemDesc, inputBuffer, (ULONG)inputBufferLen);
	WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&outputMemDesc, outputMemory, NULL);

	status = WdfIoTargetSendInternalIoctlSynchronously(
		WdfDeviceGetIoTarget(FxDevice),
		NULL,
		IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA,
		&inputMemDesc,
		&outputMemDesc,
		NULL,
		NULL
	);
	if (!NT_SUCCESS(status)) {
		GmaxPrint(
			DEBUG_LEVEL_ERROR,
			DBG_IOCTL,
			"Error getting device data - 0x%x\n",
			status);
		goto Exit;
	}

	if (outputBuffer->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1 &&
		outputBuffer->Count < 1 &&
		outputBuffer->Argument->Type != ACPI_METHOD_ARGUMENT_INTEGER &&
		outputBuffer->Argument->DataLength < 1) {
		status = STATUS_ACPI_INVALID_ARGUMENT;
		goto Exit;
	}

	if (property) {
		*property = outputBuffer->Argument->Data[0] & 0xF;
	}

Exit:
	if (inputBuffer) {
		ExFreePoolWithTag(inputBuffer, GMAX_POOL_TAG);
	}
	if (outputMemory != WDF_NO_HANDLE) {
		WdfObjectDelete(outputMemory);
	}
	return status;
}

NTSTATUS
StartCodec(
	PGMAX_CONTEXT pDevice
) {
	NTSTATUS status = STATUS_SUCCESS;
	if (!pDevice->SetUID) {
		status = STATUS_INVALID_DEVICE_STATE;
		return status;
	}

	UINT8 data = 0;
	gmax_reg_read(pDevice, pDevice->chipModel == 98512 ? MAX98512_REV_ID_0, &data);

	BOOLEAN useDefaults = FALSE;

	INT32 rightSpeaker = 1;
	if (GetPlatform() == PlatformAmberLake)
		rightSpeaker = 0;

	if (pDevice->chipModel == 98512) { //max98512
		for (int i = 0; i < sizeof(max98512_initregs) / sizeof(struct initreg); i++) {
			struct initreg regval = max98512_initregs[i];
			status = gmax_reg_write(pDevice, regval.reg, regval.val);
			if (!NT_SUCCESS(status)) {
				return status;
			}
		}

		UINT16 ampVolume = 60;
		UINT16 speakerGain = 1;

		UINT16 interleave_mode = 0;
		UINT16 vmon_slot_no = 0;
		UINT16 imon_slot_no = 0;

		if (!NT_SUCCESS(GetIntegerProperty(pDevice->FxDevice, "interleave_mode", &interleave_mode))) {
			DbgPrint("Warning: unable to get vmon-slot-no. Using defaults.\n");
			useDefaults = TRUE;
		}
		interleave_mode = interleave_mode & 1;

		if (!NT_SUCCESS(GetIntegerProperty(pDevice->FxDevice, "vmon-slot-no", &vmon_slot_no))) {
			DbgPrint("Warning: unable to get vmon-slot-no. Using defaults.\n");
			useDefaults = TRUE;
		}
		if (!NT_SUCCESS(GetIntegerProperty(pDevice->FxDevice, "imon-slot-no", &vmon_slot_no))) {
			DbgPrint("Warning: unable to get vmon-slot-no. Using defaults.\n");
			useDefaults = TRUE;
		}

		if (useDefaults) {
			interleave_mode = 0;
			if (pDevice->UID == 0) {
				vmon_slot_no = 4;
				imon_slot_no = 5;
			}
			else {
				vmon_slot_no = 6;
				imon_slot_no = 7;
			}
		}

		UINT16 temp = (1 << vmon_slot_no) | (1 << imon_slot_no);
		status = gmax_reg_write(pDevice, MAX98512_R001A_PCM_TX_EN_A, temp);
		if (!NT_SUCCESS(status)) {
			return status;
		}

		status = gmax_reg_write(pDevice, MAX98512_R001B_PCM_TX_EN_B, (temp >> 8) | 0xff00);
		if (!NT_SUCCESS(status)) {
			return status;
		}

		temp = ~temp;
		status = gmax_reg_write(pDevice, MAX98512_R001C_PCM_TX_HIZ_CTRL_A, temp);
		if (!NT_SUCCESS(status)) {
			return status;
		}

		status = gmax_reg_write(pDevice, MAX98512_R001D_PCM_TX_HIZ_CTRL_B, (temp >> 8) | 0xff00);
		if (!NT_SUCCESS(status)) {
			return status;
		}

		status = gmax_reg_write(pDevice, MAX98512_R001E_PCM_TX_CH_SRC_A, (imon_slot_no << MAX98927_PCM_TX_CH_SRC_A_I_SHIFT |
			vmon_slot_no) & 0xFF);
		if (!NT_SUCCESS(status)) {
			return status;
		}

		status = gmax_reg_write(pDevice, MAX98512_R001F_PCM_TX_CH_SRC_B, interleave_mode != 0 ? MAX98927_PCM_TX_CH_INTERLEAVE_MASK : 0);
		if (!NT_SUCCESS(status)) {
			return status;
		}

		status = gmax_reg_write(pDevice, MAX98512_R0024_PCM_SR_SETUP2, interleave_mode != 0 ? 0x85 : 0x88);
		if (!NT_SUCCESS(status)) {
			return status;
		}

		status = gmax_reg_write(pDevice, MAX98512_R0025_PCM_TO_SPK_MONOMIX_A, pDevice->UID == rightSpeaker ? 0x40 : 0);
		if (!NT_SUCCESS(status)) {
			return status;
		}

		status = gmax_reg_write(pDevice, MAX98512_R0026_PCM_TO_SPK_MONOMIX_B, 1);
		if (!NT_SUCCESS(status)) {
			return status;
		}
	}

	status = enableOutput(pDevice, TRUE);

	/*uint16_t regs[] = {0x0001,0x0002,0x0003,0x0004,0x0005,0x0006,0x0007,0x0008,0x0009,0x000A,0x000B,0x000C,0x000D,0x000E,0x000F,0x0010,0x0011,0x0012,0x0013,0x0014,0x0015,0x0016,0x0017,0x0018,0x0019,0x001A,0x001B,0x001C,0x001D,0x001E,0x001F,0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x002B,0x002C,0x002E,0x002F,0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x0051,0x0052,0x0053,0x0054,0x0055,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,0x0060,0x0061,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x007F,0x0080,0x0081,0x0082,0x0083,0x0084,0x0085,0x0086,0x0087,0x00FF,0x0100,0x01FF};
	for (int i = 0; i < sizeof(regs) / sizeof(uint16_t); i++) {
		UINT16 reg = regs[i];
		UINT8 data = 0;
		gmax_reg_read(pDevice, reg, &data);

		DbgPrint("Reg 0x%04x:\n\t0x%02x", reg, data);
	}*/

	pDevice->DevicePoweredOn = TRUE;
	return status;
}

NTSTATUS
StopCodec(
	PGMAX_CONTEXT pDevice
) {
	NTSTATUS status;

	status = gmax_reg_write(pDevice, pDevice->chipModel == 98512 ? MAX98512_SOFT_RESET, 1);
	
	pDevice->DevicePoweredOn = FALSE;
	return status;
}

VOID
CSAudioRegisterEndpoint(
	PGMAX_CONTEXT pDevice
) {
	return;

	CsAudioArg arg;
	RtlZeroMemory(&arg, sizeof(CsAudioArg));
	arg.argSz = sizeof(CsAudioArg);
	arg.endpointType = CSAudioEndpointTypeSpeaker;
	arg.endpointRequest = CSAudioEndpointRegister;
	ExNotifyCallback(pDevice->CSAudioAPICallback, &arg, &CsAudioArg2);
}

VOID
CsAudioCallbackFunction(
	IN PGMAX_CONTEXT  pDevice,
	CsAudioArg* arg,
	PVOID Argument2
) {
	if (!pDevice) {
		return;
	}

	if (Argument2 == &CsAudioArg2) {
		return;
	}

	CsAudioArg localArg;
	RtlZeroMemory(&localArg, sizeof(CsAudioArg));
	RtlCopyMemory(&localArg, arg, min(arg->argSz, sizeof(CsAudioArg)));

	if (localArg.endpointType == CSAudioEndpointTypeDSP && localArg.endpointRequest == CSAudioEndpointRegister) {
		CSAudioRegisterEndpoint(pDevice);
	}
	else if (localArg.endpointType != CSAudioEndpointTypeSpeaker) {
		return;
	}

	if (localArg.endpointRequest == CSAudioEndpointStop) {
		if (pDevice->CSAudioRequestsOn) {
			WdfDeviceResumeIdle(pDevice->FxDevice);
			pDevice->CSAudioRequestsOn = FALSE;
		}
	}
	if (localArg.endpointRequest == CSAudioEndpointStart) {
		if (!pDevice->CSAudioRequestsOn) {
			WdfDeviceStopIdle(pDevice->FxDevice, TRUE);
			pDevice->CSAudioRequestsOn = TRUE;
		}
	}
}

NTSTATUS
OnPrepareHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesRaw,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

This routine caches the SPB resource connection ID.

Arguments:

FxDevice - a handle to the framework device object
FxResourcesRaw - list of translated hardware resources that
the PnP manager has assigned to the device
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PGMAX_CONTEXT pDevice = GetDeviceContext(FxDevice);
	BOOLEAN fSpbResourceFound = FALSE;
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);

		switch (pDescriptor->Type)
		{
		case CmResourceTypeConnection:
			//
			// Look for I2C or SPI resource and save connection ID.
			//
			Class = pDescriptor->u.Connection.Class;
			Type = pDescriptor->u.Connection.Type;
			if (Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
				Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
			{
				if (fSpbResourceFound == FALSE)
				{
					status = STATUS_SUCCESS;
					pDevice->I2CContext.I2cResHubId.LowPart = pDescriptor->u.Connection.IdLowPart;
					pDevice->I2CContext.I2cResHubId.HighPart = pDescriptor->u.Connection.IdHighPart;
					fSpbResourceFound = TRUE;
				}
				else
				{
				}
			}
			break;
		default:
			//
			// Ignoring all other resource types.
			//
			break;
		}
	}

	//
	// An SPB resource is required.
	//

	if (fSpbResourceFound == FALSE)
	{
		status = STATUS_NOT_FOUND;
	}

	status = SpbTargetInitialize(FxDevice, &pDevice->I2CContext);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = GetDeviceUID(FxDevice, &pDevice->UID);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = GetDeviceHID(FxDevice);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	pDevice->SetUID = TRUE;

	return status;
}

NTSTATUS
OnReleaseHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

Arguments:

FxDevice - a handle to the framework device object
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PGMAX_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	SpbTargetDeinitialize(FxDevice, &pDevice->I2CContext);

	if (pDevice->CSAudioAPICallbackObj) {
		ExUnregisterCallback(pDevice->CSAudioAPICallbackObj);
		pDevice->CSAudioAPICallbackObj = NULL;
	}

	if (pDevice->CSAudioAPICallback) {
		ObfDereferenceObject(pDevice->CSAudioAPICallback);
		pDevice->CSAudioAPICallback = NULL;
	}

	return status;
}

NTSTATUS
OnSelfManagedIoInit(
	_In_
	WDFDEVICE FxDevice
) {
	PGMAX_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	if (!pDevice->SetUID) {
		status = STATUS_INVALID_DEVICE_STATE;
		return status;
	}

	// CS Audio Callback

	UNICODE_STRING CSAudioCallbackAPI;
	RtlInitUnicodeString(&CSAudioCallbackAPI, L"\\CallBack\\CsAudioCallbackAPI");


	OBJECT_ATTRIBUTES attributes;
	InitializeObjectAttributes(&attributes,
		&CSAudioCallbackAPI,
		OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT,
		NULL,
		NULL
	);
	status = ExCreateCallback(&pDevice->CSAudioAPICallback, &attributes, TRUE, TRUE);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	pDevice->CSAudioAPICallbackObj = ExRegisterCallback(pDevice->CSAudioAPICallback,
		CsAudioCallbackFunction,
		pDevice
	);
	if (!pDevice->CSAudioAPICallbackObj) {

		return STATUS_NO_CALLBACK_ACTIVE;
	}

	CSAudioRegisterEndpoint(pDevice);

	return status;
}

NTSTATUS
OnD0Entry(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine allocates objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PGMAX_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = StartCodec(pDevice);
	return status;
}

NTSTATUS
OnD0Exit(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine destroys objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PGMAX_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	status = StopCodec(pDevice);

	return STATUS_SUCCESS;
}

NTSTATUS
GmaxEvtDeviceAdd(
	IN WDFDRIVER       Driver,
	IN PWDFDEVICE_INIT DeviceInit
)
{
	NTSTATUS                      status = STATUS_SUCCESS;
	WDF_IO_QUEUE_CONFIG           queueConfig;
	WDF_OBJECT_ATTRIBUTES         attributes;
	WDFDEVICE                     device;
	WDFQUEUE                      queue;
	PGMAX_CONTEXT               devContext;

	UNREFERENCED_PARAMETER(Driver);

	PAGED_CODE();

	GmaxPrint(DEBUG_LEVEL_INFO, DBG_PNP,
		"GmaxEvtDeviceAdd called\n");

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

		pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
		pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
		pnpCallbacks.EvtDeviceSelfManagedIoInit = OnSelfManagedIoInit;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
	}

	//
	// Setup the device context
	//

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, GMAX_CONTEXT);

	//
	// Create a framework device object.This call will in turn create
	// a WDM device object, attach to the lower stack, and set the
	// appropriate flags and attributes.
	//

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);

	if (!NT_SUCCESS(status))
	{
		GmaxPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceCreate failed with status code 0x%x\n", status);

		return status;
	}

	{
		WDF_DEVICE_STATE deviceState;
		WDF_DEVICE_STATE_INIT(&deviceState);

		deviceState.NotDisableable = WdfFalse;
		WdfDeviceSetDeviceState(device, &deviceState);
	}

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

	queueConfig.EvtIoInternalDeviceControl = GmaxEvtInternalDeviceControl;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&queue
	);

	if (!NT_SUCCESS(status))
	{
		GmaxPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create manual I/O queue to take care of hid report read requests
	//

	devContext = GetDeviceContext(device);

	devContext->FxDevice = device;

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfTrue;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->ReportQueue
	);

	if (!NT_SUCCESS(status))
	{
		GmaxPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS IdleSettings;

	WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings, IdleCannotWakeFromS0);
	IdleSettings.IdleTimeoutType = SystemManagedIdleTimeoutWithHint;
	IdleSettings.IdleTimeout = 1000;
	IdleSettings.Enabled = WdfTrue;

	WdfDeviceAssignS0IdleSettings(devContext->FxDevice, &IdleSettings);

	return status;
}

VOID
GmaxEvtInternalDeviceControl(
	IN WDFQUEUE     Queue,
	IN WDFREQUEST   Request,
	IN size_t       OutputBufferLength,
	IN size_t       InputBufferLength,
	IN ULONG        IoControlCode
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	WDFDEVICE           device;
	PGMAX_CONTEXT     devContext;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	device = WdfIoQueueGetDevice(Queue);
	devContext = GetDeviceContext(device);

	switch (IoControlCode)
	{
	default:
		status = STATUS_NOT_SUPPORTED;
		break;
	}

	WdfRequestComplete(Request, status);

	return;
}
