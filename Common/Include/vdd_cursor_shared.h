#pragma once

#include <stddef.h>
#include <stdint.h>

#define VDD_CURSOR_MAGIC 0x5A564355u /* 'ZVCU' */
#define VDD_CURSOR_VERSION 1u
#define VDD_CURSOR_MAX_WIDTH 256u
#define VDD_CURSOR_MAX_HEIGHT 256u
#define VDD_CURSOR_MAX_BYTES (VDD_CURSOR_MAX_WIDTH * VDD_CURSOR_MAX_HEIGHT * 4u)

/*
 * Shared-memory cursor metadata followed immediately by ShapeBufferSize bytes
 * of cursor pixels. PublicationSequence is odd while the producer is writing
 * and even when the snapshot is stable. ShapeId and PositionId are published
 * after their associated data so legacy readers can continue using the IDs as
 * lightweight commit markers.
 */
#pragma pack(push, 4)
typedef struct VDD_CURSOR_SHARED_METADATA
{
	uint32_t Magic;
	uint32_t Version;
	uint32_t IsVisible;
	int32_t PositionX;
	int32_t PositionY;
	uint32_t PositionId;
	uint32_t ShapeId;
	uint32_t ShapeType;
	uint32_t Width;
	uint32_t Height;
	uint32_t Pitch;
	int32_t XHot;
	int32_t YHot;
	uint32_t SdrWhiteLevelX1000;
	uint32_t ShapeBufferSize;
	uint32_t PublicationSequence;
	uint64_t LastUpdateQpc;
} VDD_CURSOR_SHARED_METADATA;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(VDD_CURSOR_SHARED_METADATA) == 72, "Unexpected cursor metadata layout");
static_assert(offsetof(VDD_CURSOR_SHARED_METADATA, PublicationSequence) == 60, "Unexpected cursor sequence offset");
static_assert(offsetof(VDD_CURSOR_SHARED_METADATA, LastUpdateQpc) == 64, "Unexpected cursor QPC offset");
#endif
