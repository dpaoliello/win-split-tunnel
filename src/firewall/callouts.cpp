#include "wfp.h"
#include "firewall.h"
#include "context.h"
#include "identifiers.h"
#include "splitting.h"
#include "asyncbind.h"
#include "callouts.h"

namespace firewall
{

namespace
{

//
// NotifyFilterAttach()
//
// Receive notifications about filters attaching/detaching the callout.
//
NTSTATUS
NotifyFilterAttach
(
	FWPS_CALLOUT_NOTIFY_TYPE notifyType,
	const GUID *filterKey,
	FWPS_FILTER1 *filter
)
{
	UNREFERENCED_PARAMETER(notifyType);
	UNREFERENCED_PARAMETER(filterKey);
	UNREFERENCED_PARAMETER(filter);

	return STATUS_SUCCESS;
}

NTSTATUS
RegisterCalloutTx
(
	PDEVICE_OBJECT DeviceObject,
	HANDLE WfpSession,
	FWPS_CALLOUT_CLASSIFY_FN1 Callout,
	const GUID *CalloutKey,
	const GUID *LayerKey,
	const wchar_t *CalloutName,
	const wchar_t* CalloutDescription
)
{
	//
	// Logically, this is the wrong order, but it results in cleaner code.
	// You're encouraged to first register the callout and then add it.
	//
	// However, what's currently here is fully supported:
	//
	// `By default filters that reference callouts that have been added
	// but have not yet registered with the filter engine are treated as Block filters.`
	//

	FWPM_CALLOUT0 callout;

	RtlZeroMemory(&callout, sizeof(callout));

	callout.calloutKey = *CalloutKey;
	callout.displayData.name = const_cast<wchar_t *>(CalloutName);
	callout.displayData.description = const_cast<wchar_t *>(CalloutDescription);
	callout.flags = FWPM_CALLOUT_FLAG_USES_PROVIDER_CONTEXT;
	callout.providerKey = const_cast<GUID *>(&ST_FW_PROVIDER_KEY);
	callout.applicableLayer = *LayerKey;

	auto status = FwpmCalloutAdd0(WfpSession, &callout, NULL, NULL);

    if (!NT_SUCCESS(status))
    {
		return status;
	}

    FWPS_CALLOUT1 aCallout = { 0 };

    aCallout.calloutKey = *CalloutKey;
    aCallout.classifyFn = Callout;
    aCallout.notifyFn = NotifyFilterAttach;
    aCallout.flowDeleteFn = NULL;

    return FwpsCalloutRegister1(DeviceObject, &aCallout, NULL);
}

void
ClassifyUnknownBind
(
	CONTEXT *Context,
	HANDLE ProcessId,
	UINT64 FilterId,
	const void *ClassifyContext,
	FWPS_CLASSIFY_OUT0 *ClassifyOut,
	bool Ipv4
)
{
	//
	// Pend the bind and wait for process to become known and classified.
	//

	auto status = PendBindRequest
	(
		Context,
		ProcessId,
		const_cast<void*>(ClassifyContext),
		FilterId,
		ClassifyOut,
		Ipv4
	);

	if (NT_SUCCESS(status))
	{
		return;
	}

	DbgPrint("Could not pend bind request from process %p, blocking instead\n", ProcessId);

	FailBindRequest
	(
		ProcessId,
		const_cast<void*>(ClassifyContext),
		FilterId,
		ClassifyOut,
		Ipv4
	);
}

//
// CalloutClassifyBind()
//
// Entry point for splitting traffic.
// Check whether the binding process is marked for having its traffic split.
//
// FWPS_LAYER_ALE_BIND_REDIRECT_V4
// FWPS_LAYER_ALE_BIND_REDIRECT_V6
//
void
CalloutClassifyBind
(
	const FWPS_INCOMING_VALUES0 *FixedValues,
	const FWPS_INCOMING_METADATA_VALUES0 *MetaValues,
	void *LayerData,
	const void *ClassifyContext,
	const FWPS_FILTER1 *Filter,
	UINT64 FlowContext,
	FWPS_CLASSIFY_OUT0 *ClassifyOut
)
{
	UNREFERENCED_PARAMETER(LayerData);
	UNREFERENCED_PARAMETER(FlowContext);

	NT_ASSERT
	(
		FixedValues->layerId == FWPS_LAYER_ALE_BIND_REDIRECT_V4
			|| FixedValues->layerId == FWPS_LAYER_ALE_BIND_REDIRECT_V6
	);

	NT_ASSERT
	(
		Filter->providerContext != NULL
		&& Filter->providerContext->type == FWPM_GENERAL_CONTEXT
		&& Filter->providerContext->dataBuffer->size == sizeof(CONTEXT*)
	);

	auto context = *(CONTEXT**)Filter->providerContext->dataBuffer->data;

	if (0 == (ClassifyOut->rights & FWPS_RIGHT_ACTION_WRITE))
	{
		DbgPrint("Aborting bind processing because hard permit/block already applied\n");

		return;
	}

	if (ClassifyOut->actionType == FWP_ACTION_NONE)
	{
		ClassifyOut->actionType = FWP_ACTION_CONTINUE;
	}

	if (!FWPS_IS_METADATA_FIELD_PRESENT(MetaValues, FWPS_METADATA_FIELD_PROCESS_ID))
	{
		DbgPrint("Failed to classify bind because PID was not provided\n");

		return;
	}

	const CALLBACKS &callbacks = context->Callbacks;

	const auto verdict = callbacks.QueryProcess(HANDLE(MetaValues->processId), callbacks.Context);

	switch (verdict)
	{
		case PROCESS_SPLIT_VERDICT::DO_SPLIT:
		{
			RewriteBind
			(
				context,
				FixedValues,
				MetaValues,
				Filter->filterId,
				ClassifyContext,
				ClassifyOut
			);

			break;
		}
		case PROCESS_SPLIT_VERDICT::UNKNOWN:
		{
			ClassifyUnknownBind
			(
				context,
				HANDLE(MetaValues->processId),
				Filter->filterId,
				ClassifyContext,
				ClassifyOut,
				FixedValues->layerId == FWPS_LAYER_ALE_BIND_REDIRECT_V4
			);

			break;
		}
	};
}

bool IsAleReauthorize
(
	const FWPS_INCOMING_VALUES *FixedValues
)
{
	size_t index;

	switch (FixedValues->layerId)
	{
		case FWPS_LAYER_ALE_AUTH_CONNECT_V4:
		{
			index = FWPS_FIELD_ALE_AUTH_CONNECT_V4_FLAGS;
			break;
		}
		case FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V4:
		{
			index = FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V4_FLAGS;
			break;
		}
		case FWPS_LAYER_ALE_AUTH_CONNECT_V6:
		{
			index = FWPS_FIELD_ALE_AUTH_CONNECT_V6_FLAGS;
			break;
		}
		case FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V6:
		{
			index = FWPS_FIELD_ALE_AUTH_RECV_ACCEPT_V6_FLAGS;
			break;
		}
		default:
		{
			return false;
		}
	};

	const auto flags = FixedValues->incomingValue[index].value.uint32;

	return ((flags & FWP_CONDITION_FLAG_IS_REAUTHORIZE) != 0);
}

//
// CalloutPermitSplitApps()
//
// For processes being split, the bind will have already been moved off the
// tunnel interface.
//
// So now it's only a matter of approving the connection.
//
// FWPS_LAYER_ALE_AUTH_CONNECT_V4
// FWPS_LAYER_ALE_AUTH_CONNECT_V6
// FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V4
// FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V6
//
void
CalloutPermitSplitApps
(
	const FWPS_INCOMING_VALUES0 *FixedValues,
	const FWPS_INCOMING_METADATA_VALUES0 *MetaValues,
	void *LayerData,
	const void *ClassifyContext,
	const FWPS_FILTER1 *Filter,
	UINT64 FlowContext,
	FWPS_CLASSIFY_OUT0 *ClassifyOut
)
{
#if !DBG
	UNREFERENCED_PARAMETER(FixedValues);
#endif
	UNREFERENCED_PARAMETER(LayerData);
	UNREFERENCED_PARAMETER(ClassifyContext);
	UNREFERENCED_PARAMETER(Filter);
	UNREFERENCED_PARAMETER(FlowContext);

	NT_ASSERT
	(
		FixedValues->layerId == FWPS_LAYER_ALE_AUTH_CONNECT_V4
			|| FixedValues->layerId == FWPS_LAYER_ALE_AUTH_CONNECT_V6
			|| FixedValues->layerId == FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V4
			|| FixedValues->layerId == FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V6
	);

	NT_ASSERT
	(
		Filter->providerContext != NULL
		&& Filter->providerContext->type == FWPM_GENERAL_CONTEXT
		&& Filter->providerContext->dataBuffer->size == sizeof(CONTEXT*)
	);

	auto context = *(CONTEXT**)Filter->providerContext->dataBuffer->data;

	if (0 == (ClassifyOut->rights & FWPS_RIGHT_ACTION_WRITE))
	{
		DbgPrint("Aborting connection processing because hard permit/block already applied\n");

		return;
	}

	if (ClassifyOut->actionType == FWP_ACTION_NONE)
	{
		ClassifyOut->actionType = FWP_ACTION_CONTINUE;
	}

	if (!FWPS_IS_METADATA_FIELD_PRESENT(MetaValues, FWPS_METADATA_FIELD_PROCESS_ID))
	{
		DbgPrint("Failed to classify connection because PID was not provided\n");

		return;
	}

	const CALLBACKS &callbacks = context->Callbacks;

	const auto verdict = callbacks.QueryProcess(HANDLE(MetaValues->processId), callbacks.Context);

	if (verdict == PROCESS_SPLIT_VERDICT::DO_SPLIT)
	{
		DbgPrint("APPROVING CONNECTION\n");

		ClassifyOut->actionType = FWP_ACTION_PERMIT;
		ClassifyOut->rights &= ~FWPS_RIGHT_ACTION_WRITE;
	}
	else
	{
#if DBG
		if (IsAleReauthorize(FixedValues))
		{
			DbgPrint("[CalloutPermitSplitApps] Reauthorized connection (PID: %p) is not explicitly "\
				"approved by callout\n", HANDLE(MetaValues->processId));
		}
#endif
	}
}

//
// CalloutBlockSplitApps()
//
// For processes just now being split, it could be the case that they have existing
// long-lived connections inside the tunnel.
//
// These connections need to be blocked to ensure the process exists on
// only one side of the tunnel.
//
// FWPS_LAYER_ALE_AUTH_CONNECT_V4
// FWPS_LAYER_ALE_AUTH_CONNECT_V6
// FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V4
// FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V6
//
void
CalloutBlockSplitApps
(
	const FWPS_INCOMING_VALUES0 *FixedValues,
	const FWPS_INCOMING_METADATA_VALUES0 *MetaValues,
	void *LayerData,
	const void *ClassifyContext,
	const FWPS_FILTER1 *Filter,
	UINT64 FlowContext,
	FWPS_CLASSIFY_OUT0 *ClassifyOut
)
{
#if !DBG
	UNREFERENCED_PARAMETER(FixedValues);
#endif
	UNREFERENCED_PARAMETER(LayerData);
	UNREFERENCED_PARAMETER(ClassifyContext);
	UNREFERENCED_PARAMETER(Filter);
	UNREFERENCED_PARAMETER(FlowContext);

	NT_ASSERT
	(
		FixedValues->layerId == FWPS_LAYER_ALE_AUTH_CONNECT_V4
			|| FixedValues->layerId == FWPS_LAYER_ALE_AUTH_CONNECT_V6
			|| FixedValues->layerId == FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V4
			|| FixedValues->layerId == FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V6
	);

	NT_ASSERT
	(
		Filter->providerContext != NULL
		&& Filter->providerContext->type == FWPM_GENERAL_CONTEXT
		&& Filter->providerContext->dataBuffer->size == sizeof(CONTEXT*)
	);

	auto context = *(CONTEXT**)Filter->providerContext->dataBuffer->data;

	if (0 == (ClassifyOut->rights & FWPS_RIGHT_ACTION_WRITE))
	{
		DbgPrint("Aborting connection processing because hard permit/block already applied\n");

		return;
	}

	if (ClassifyOut->actionType == FWP_ACTION_NONE)
	{
		ClassifyOut->actionType = FWP_ACTION_CONTINUE;
	}

	if (!FWPS_IS_METADATA_FIELD_PRESENT(MetaValues, FWPS_METADATA_FIELD_PROCESS_ID))
	{
		DbgPrint("Failed to classify connection because PID was not provided\n");

		return;
	}

	const CALLBACKS &callbacks = context->Callbacks;

	const auto verdict = callbacks.QueryProcess(HANDLE(MetaValues->processId), callbacks.Context);

	if (verdict == PROCESS_SPLIT_VERDICT::DO_SPLIT)
	{
		DbgPrint("BLOCKING CONNECTION\n");

		ClassifyOut->actionType = FWP_ACTION_BLOCK;
		ClassifyOut->rights &= ~FWPS_RIGHT_ACTION_WRITE;
	}
	else
	{
#if DBG
		if (IsAleReauthorize(FixedValues))
		{
			DbgPrint("[CalloutBlockSplitApps] Reauthorized connection (PID: %p) is not explicitly "\
				"blocked by callout\n", HANDLE(MetaValues->processId));
		}
#endif
	}
}

} // anonymous namespace

//
// RegisterCalloutClassifyBindTx()
//
// Register callout with WFP. In all applicable layers.
//
// "Tx" (in transaction) suffix means there is no clean-up in failure paths.
//
NTSTATUS
RegisterCalloutClassifyBindTx
(
	PDEVICE_OBJECT DeviceObject,
	HANDLE WfpSession
)
{
	auto status = RegisterCalloutTx
	(
		DeviceObject,
		WfpSession,
		CalloutClassifyBind,
		&ST_FW_CALLOUT_CLASSIFY_BIND_IPV4_KEY,
		&FWPM_LAYER_ALE_BIND_REDIRECT_V4,
		L"Mullvad Split Tunnel Bind Redirect Callout (IPv4)",
		L"Redirects certain binds away from tunnel interface"
	);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = RegisterCalloutTx
	(
		DeviceObject,
		WfpSession,
		CalloutClassifyBind,
		&ST_FW_CALLOUT_CLASSIFY_BIND_IPV6_KEY,
		&FWPM_LAYER_ALE_BIND_REDIRECT_V6,
		L"Mullvad Split Tunnel Bind Redirect Callout (IPv6)",
		L"Redirects certain binds away from tunnel interface"
	);

	if (!NT_SUCCESS(status))
	{
		UnregisterCalloutClassifyBind();
	}

	return status;
}

NTSTATUS
UnregisterCalloutClassifyBind
(
)
{
#define RETURN_IF_FAILED(status) \
	if (!NT_SUCCESS(status) && status != STATUS_FWP_CALLOUT_NOT_FOUND) \
	{ \
		return status; \
	}

    auto s1 = FwpsCalloutUnregisterByKey0(&ST_FW_CALLOUT_CLASSIFY_BIND_IPV4_KEY);
	auto s2 = FwpsCalloutUnregisterByKey0(&ST_FW_CALLOUT_CLASSIFY_BIND_IPV6_KEY);

	RETURN_IF_FAILED(s1)
	RETURN_IF_FAILED(s2)

	return STATUS_SUCCESS;
}

//
// RegisterCalloutPermitSplitAppsTx()
//
// Register callout with WFP. In all applicable layers.
//
// "Tx" (in transaction) suffix means there is no clean-up in failure paths.
//
NTSTATUS
RegisterCalloutPermitSplitAppsTx
(
	PDEVICE_OBJECT DeviceObject,
	HANDLE WfpSession
)
{
	auto status = RegisterCalloutTx
	(
		DeviceObject,
		WfpSession,
		CalloutPermitSplitApps,
		&ST_FW_CALLOUT_PERMIT_SPLIT_APPS_IPV4_CONN_KEY,
		&FWPM_LAYER_ALE_AUTH_CONNECT_V4,
		L"Mullvad Split Tunnel Permitting Callout (IPv4)",
		L"Permits selected connections outside the tunnel"
	);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = RegisterCalloutTx
	(
		DeviceObject,
		WfpSession,
		CalloutPermitSplitApps,
		&ST_FW_CALLOUT_PERMIT_SPLIT_APPS_IPV4_RECV_KEY,
		&FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
		L"Mullvad Split Tunnel Permitting Callout (IPv4)",
		L"Permits selected connections outside the tunnel"
	);

	if (!NT_SUCCESS(status))
	{
		UnregisterCalloutPermitSplitApps();

		return status;
	}

	status = RegisterCalloutTx
	(
		DeviceObject,
		WfpSession,
		CalloutPermitSplitApps,
		&ST_FW_CALLOUT_PERMIT_SPLIT_APPS_IPV6_CONN_KEY,
		&FWPM_LAYER_ALE_AUTH_CONNECT_V6,
		L"Mullvad Split Tunnel Permitting Callout (IPv6)",
		L"Permits selected connections outside the tunnel"
	);

	if (!NT_SUCCESS(status))
	{
		UnregisterCalloutPermitSplitApps();

		return status;
	}

	status = RegisterCalloutTx
	(
		DeviceObject,
		WfpSession,
		CalloutPermitSplitApps,
		&ST_FW_CALLOUT_PERMIT_SPLIT_APPS_IPV6_RECV_KEY,
		&FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6,
		L"Mullvad Split Tunnel Permitting Callout (IPv6)",
		L"Permits selected connections outside the tunnel"
	);

	if (!NT_SUCCESS(status))
	{
		UnregisterCalloutPermitSplitApps();

		return status;
	}

	return STATUS_SUCCESS;
}

NTSTATUS
UnregisterCalloutPermitSplitApps
(
)
{
#define RETURN_IF_FAILED(status) \
	if (!NT_SUCCESS(status) && status != STATUS_FWP_CALLOUT_NOT_FOUND) \
	{ \
		return status; \
	}

    auto s1 = FwpsCalloutUnregisterByKey0(&ST_FW_CALLOUT_PERMIT_SPLIT_APPS_IPV4_CONN_KEY);
	auto s2 = FwpsCalloutUnregisterByKey0(&ST_FW_CALLOUT_PERMIT_SPLIT_APPS_IPV4_RECV_KEY);
    auto s3 = FwpsCalloutUnregisterByKey0(&ST_FW_CALLOUT_PERMIT_SPLIT_APPS_IPV6_CONN_KEY);
	auto s4 = FwpsCalloutUnregisterByKey0(&ST_FW_CALLOUT_PERMIT_SPLIT_APPS_IPV6_RECV_KEY);

	RETURN_IF_FAILED(s1);
	RETURN_IF_FAILED(s2);
	RETURN_IF_FAILED(s3);
	RETURN_IF_FAILED(s4);

	return STATUS_SUCCESS;
}

//
// RegisterCalloutBlockSplitAppsTx()
//
// Register callout with WFP. In all applicable layers.
//
// "Tx" (in transaction) suffix means there is no clean-up in failure paths.
//
NTSTATUS
RegisterCalloutBlockSplitAppsTx
(
	PDEVICE_OBJECT DeviceObject,
	HANDLE WfpSession
)
{
	auto status = RegisterCalloutTx
	(
		DeviceObject,
		WfpSession,
		CalloutBlockSplitApps,
		&ST_FW_CALLOUT_BLOCK_SPLIT_APPS_IPV4_CONN_KEY,
		&FWPM_LAYER_ALE_AUTH_CONNECT_V4,
		L"Mullvad Split Tunnel Blocking Callout (IPv4)",
		L"Blocks unwanted connections in relation to splitting"
	);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = RegisterCalloutTx
	(
		DeviceObject,
		WfpSession,
		CalloutBlockSplitApps,
		&ST_FW_CALLOUT_BLOCK_SPLIT_APPS_IPV4_RECV_KEY,
		&FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
		L"Mullvad Split Tunnel Blocking Callout (IPv4)",
		L"Blocks unwanted connections in relation to splitting"
	);

	if (!NT_SUCCESS(status))
	{
		UnregisterCalloutBlockSplitApps();

		return status;
	}

	status = RegisterCalloutTx
	(
		DeviceObject,
		WfpSession,
		CalloutBlockSplitApps,
		&ST_FW_CALLOUT_BLOCK_SPLIT_APPS_IPV6_CONN_KEY,
		&FWPM_LAYER_ALE_AUTH_CONNECT_V6,
		L"Mullvad Split Tunnel Blocking Callout (IPv6)",
		L"Blocks unwanted connections in relation to splitting"
	);

	if (!NT_SUCCESS(status))
	{
		UnregisterCalloutBlockSplitApps();

		return status;
	}

	status = RegisterCalloutTx
	(
		DeviceObject,
		WfpSession,
		CalloutBlockSplitApps,
		&ST_FW_CALLOUT_BLOCK_SPLIT_APPS_IPV6_RECV_KEY,
		&FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6,
		L"Mullvad Split Tunnel Blocking Callout (IPv6)",
		L"Blocks unwanted connections in relation to splitting"
	);

	if (!NT_SUCCESS(status))
	{
		UnregisterCalloutBlockSplitApps();

		return status;
	}

	return STATUS_SUCCESS;
}

NTSTATUS
UnregisterCalloutBlockSplitApps
(
)
{
#define RETURN_IF_FAILED(status) \
	if (!NT_SUCCESS(status) && status != STATUS_FWP_CALLOUT_NOT_FOUND) \
	{ \
		return status; \
	}

    auto s1 = FwpsCalloutUnregisterByKey0(&ST_FW_CALLOUT_BLOCK_SPLIT_APPS_IPV4_CONN_KEY);
	auto s2 = FwpsCalloutUnregisterByKey0(&ST_FW_CALLOUT_BLOCK_SPLIT_APPS_IPV4_RECV_KEY);
    auto s3 = FwpsCalloutUnregisterByKey0(&ST_FW_CALLOUT_BLOCK_SPLIT_APPS_IPV6_CONN_KEY);
	auto s4 = FwpsCalloutUnregisterByKey0(&ST_FW_CALLOUT_BLOCK_SPLIT_APPS_IPV6_RECV_KEY);

	RETURN_IF_FAILED(s1);
	RETURN_IF_FAILED(s2);
	RETURN_IF_FAILED(s3);
	RETURN_IF_FAILED(s4);

	return STATUS_SUCCESS;
}

} // namespace firewall
