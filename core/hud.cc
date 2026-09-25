#include "hud.hh"

#include <Windows.h>
#include <MinHook.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "log.hh"
#include "scan.hh"

namespace hud
{
	namespace
	{
		// Scaleform::GFx::Movie::Invoke(const char*, Value*, const Value*, unsigned):
		// mov rcx, [rcx+18h] (pASMovieRoot); mov r10, [rcx]; jmp [r10+1C0h] (ASMovieRootBase::Invoke_2).
		constexpr char kSigMovieInvoke[] = "48 8B 49 18 4C 8B 11 49 FF A2 C0 01 00 00";

		constexpr size_t kMovieASRoot = 0x18;         // Movie::pASMovieRoot
		constexpr size_t kRootGetVariable = 0x188;    // ASMovieRootBase_vtbl
		constexpr size_t kIfaceObjectRelease = 0x10;  // Value::ObjectInterface_vtbl
		constexpr size_t kIfaceGetCxform = 0x110;
		constexpr size_t kIfaceSetCxform = 0x118;

		constexpr uint32_t kTypeString = 6;
		constexpr uint32_t kTypeDisplayObject = 10;
		constexpr uint32_t kTypeManaged = 0x40; // holds a reference the owner must release

		// Scaleform::GFx::Value (0x30): a list node, then the interface, type and payload.
		struct Value
		{
			void* mPrev = nullptr;
			void* mNext = nullptr;
			void* mInterface = nullptr;
			uint32_t mType = 0; // VT_Undefined
			uint32_t mPad = 0;
			void* mData = nullptr; // const char* for strings
			uint64_t mDataAux = 0;
		};
		static_assert(sizeof(Value) == 0x30);

		// Scaleform::Render::Cxform: multiply row, then add row (R, G, B, A; add in 0..1).
		struct Cxform
		{
			float mMult[4];
			float mAdd[4];
		};

		using InvokeFn = bool(__fastcall*)(void* movie, const char* method, Value* result, const Value* args, unsigned count);
		using GetVariableFn = bool(__fastcall*)(void* root, Value* out, const char* path);
		using ReleaseFn = void(__fastcall*)(void* iface, Value* value, void* data);
		using CxformFn = bool(__fastcall*)(void* iface, void* data, Cxform* cx);

		InvokeFn gInvoke = nullptr;
		std::string gOurTexture;
		Cxform gOriginal{};
		bool gHaveOriginal = false;

		template <typename T>
		T Slot(void* object, size_t offset)
		{
			return reinterpret_cast<T>((*static_cast<void***>(object))[offset / sizeof(void*)]);
		}

		// On the game's UI thread, right after it set the logo: our cover untinted, the others as designed.
		void SetHolderTint(void* movie, bool ours)
		{
			void* root = *reinterpret_cast<void**>(static_cast<uint8_t*>(movie) + kMovieASRoot);
			if (!root) {
				return;
			}
			Value holder;
			if (!Slot<GetVariableFn>(root, kRootGetVariable)(root, &holder, "mc_RadioStations.slot.holder")) {
				LOG("hud: mc_RadioStations.slot.holder not found");
				return;
			}
			if ((holder.mType & 0x3F) != kTypeDisplayObject || !holder.mInterface) {
				LOG("hud: mc_RadioStations.slot.holder is type %u, not a display object", holder.mType);
			}
			else {
				void* iface = holder.mInterface;
				if (!gHaveOriginal) {
					gHaveOriginal = Slot<CxformFn>(iface, kIfaceGetCxform)(iface, holder.mData, &gOriginal);
					LOG("hud: holder's own color transform: mult %.2f %.2f %.2f %.2f, add %.2f %.2f %.2f %.2f%s", gOriginal.mMult[0], gOriginal.mMult[1],
						gOriginal.mMult[2], gOriginal.mMult[3], gOriginal.mAdd[0], gOriginal.mAdd[1], gOriginal.mAdd[2], gOriginal.mAdd[3],
						gHaveOriginal ? "" : " (GetCxform FAILED)");
				}
				Cxform identity{ { 1, 1, 1, 1 }, { 0, 0, 0, 0 } };
				Cxform* wanted = ours ? &identity : (gHaveOriginal ? &gOriginal : nullptr);
				if (wanted) {
					const bool set = Slot<CxformFn>(iface, kIfaceSetCxform)(iface, holder.mData, wanted);
					LOG("hud: logo %s%s", ours ? "untinted (cover art)" : "tint restored", set ? "" : " -- SetCxform FAILED");
				}
			}
			if ((holder.mType & kTypeManaged) && holder.mInterface) {
				Slot<ReleaseFn>(holder.mInterface, kIfaceObjectRelease)(holder.mInterface, &holder, holder.mData);
			}
		}

		void AfterInvoke(void* movie, const char* method, const Value* args, unsigned count)
		{
			if (movie && method && std::strcmp(method, "mc_RadioStations.SetTexture") == 0 && args && count >= 1 &&
				(args[0].mType & 0x3F) == kTypeString && args[0].mData) {
				const char* texture = static_cast<const char*>(args[0].mData);
				SetHolderTint(movie, _stricmp(texture, gOurTexture.c_str()) == 0);
			}
		}

		// Everything here runs on Scaleform internals we only know from the legacy PDB: a fault must cost the
		// cover's colors, not the game. crash.cc has logged where it happened by the time this catches it.
		bool gBroken = false;

		bool Guarded(void* movie, const char* method, const Value* args, unsigned count)
		{
			__try {
				AfterInvoke(movie, method, args, count);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool __fastcall InvokeHook(void* movie, const char* method, Value* result, const Value* args, unsigned count)
		{
			const bool ok = gInvoke(movie, method, result, args, count);
			if (!gBroken && !Guarded(movie, method, args, count)) {
				gBroken = true;
				LOG("hud: exception in the color-transform code (see the crash: lines above); cover art stays tinted from now on");
			}
			return ok;
		}
	}

	bool Install(const char* ourTexture)
	{
		gOurTexture = ourTexture;
		uint8_t* invoke = scan::FindUnique("Scaleform::GFx::Movie::Invoke", kSigMovieInvoke);
		if (!invoke) {
			return false;
		}
		MH_STATUS status = MH_CreateHook(invoke, reinterpret_cast<void*>(&InvokeHook), reinterpret_cast<void**>(&gInvoke));
		if (status == MH_OK) {
			status = MH_EnableHook(invoke);
		}
		if (status != MH_OK) {
			LOG("hud: hooking Movie::Invoke failed: %s", MH_StatusToString(status));
			return false;
		}
		return true;
	}
}
