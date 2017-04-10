// 2016 Dominic Szablewski - phoboslab.org

#include "quakedef.h"
#include "vr.h"
#include "vr_menu.h"

#define UNICODE 1
#include <mmsystem.h>
#undef UNICODE

#include "OVR_CAPI_GL.h"
#include "OVR_CAPI_Audio.h"
#include "../../openvr/headers/openvr_capi_fixed.h"
#include "lodepng.h"
#include "Matrices.h"
#include "pathtools.h"

#if SDL_MAJOR_VERSION < 2
FILE *__iob_func() {
  FILE result[3] = { *stdin,*stdout,*stderr };
  return result;
}
#endif

extern void VID_Refocus();

typedef struct {
	GLuint framebuffer, depth_texture, m_nResolveTextureId;
	//ovrTextureSwapChain swap_chain;
	struct {
		float width, height;
	} size;
} fbo_t;

typedef struct {
	int index;
	fbo_t fbo;
	Vector3 HmdToEyeOffset;
	Matrix4 pose;
	Matrix4 proj;
	float fov_x, fov_y;
} vr_eye_t;


// OpenGL Extensions
#define GL_READ_FRAMEBUFFER_EXT 0x8CA8
#define GL_DRAW_FRAMEBUFFER_EXT 0x8CA9
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9

typedef void (APIENTRYP PFNGLBLITFRAMEBUFFEREXTPROC) (GLint,  GLint,  GLint,  GLint,  GLint,  GLint,  GLint,  GLint,  GLbitfield,  GLenum);
typedef BOOL (APIENTRYP PFNWGLSWAPINTERVALEXTPROC) (int);

static PFNGLBINDFRAMEBUFFEREXTPROC glBindFramebufferEXT;
static PFNGLBLITFRAMEBUFFEREXTPROC glBlitFramebufferEXT;
static PFNGLDELETEFRAMEBUFFERSEXTPROC glDeleteFramebuffersEXT;
static PFNGLGENFRAMEBUFFERSEXTPROC glGenFramebuffersEXT;
static PFNGLFRAMEBUFFERTEXTURE2DEXTPROC glFramebufferTexture2DEXT;
static PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC glFramebufferRenderbufferEXT;
static PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT;

struct {
	void *func; char *name;
} gl_extensions[] = {
	{&glBindFramebufferEXT, "glBindFramebufferEXT"},
	{&glBlitFramebufferEXT, "glBlitFramebufferEXT"},
	{&glDeleteFramebuffersEXT, "glDeleteFramebuffersEXT"},
	{&glGenFramebuffersEXT, "glGenFramebuffersEXT"},
	{&glFramebufferTexture2DEXT, "glFramebufferTexture2DEXT"},
	{&glFramebufferRenderbufferEXT, "glFramebufferRenderbufferEXT"},
	{&wglSwapIntervalEXT, "wglSwapIntervalEXT"},
	{NULL, NULL},
};

// main screen & 2D drawing
extern void SCR_SetUpToDrawConsole (void);
extern void SCR_UpdateScreenContent();
extern qboolean	scr_drawdialog;
extern void SCR_DrawNotifyString (void);
extern qboolean	scr_drawloading;
extern void SCR_DrawLoading (void);
extern void SCR_CheckDrawCenterString (void);
extern void SCR_DrawRam (void);
extern void SCR_DrawNet (void);
extern void SCR_DrawTurtle (void);
extern void SCR_DrawPause (void);
extern void SCR_DrawDevStats (void);
extern void SCR_DrawFPS (void);
extern void SCR_DrawClock (void);
extern void SCR_DrawConsole (void);

// rendering
extern void R_SetupView(void);
extern void R_RenderScene(void);
extern int glx, gly, glwidth, glheight;
extern refdef_t r_refdef;
extern vec3_t vright;


static ovrSession session;
static ovrHmdDesc hmd;

//openvr stuff
//k_unMaxTrackedDeviceCount
#define MAX_TRACKED_DEVICE_COUNT 16

VR_IVRSystem* m_pHMD;
VR_IVRRenderModels* m_pRenderModels;
VR_IVRCompositor* m_pCompositor;
TrackedDevicePose_t m_rTrackedDevicePose[MAX_TRACKED_DEVICE_COUNT];

char* m_strDriver;
char* m_strDisplay;



Matrix4 m_mat4HMDPose;
Matrix4 m_mat4eyePosLeft;
Matrix4 m_mat4eyePosRight;

Matrix4 m_mat4ProjectionCenter;
Matrix4 m_mat4ProjectionLeft;
Matrix4 m_mat4ProjectionRight;

float m_fNearClip = 0.1f;
//float m_fFarClip = 30.0f;

uint32_t m_nRenderWidth;
uint32_t m_nRenderHeight;

char* m_strPoseClasses;                            // what classes we saw poses for this frame
char m_rDevClassChar[MAX_TRACKED_DEVICE_COUNT];   // for each device, a character representing its class


//OPENVR
//TrackedDevicePose_t m_rTrackedDevicePose[MAX_TRACKED_DEVICE_COUNT];
Matrix4 m_rmat4DevicePose[MAX_TRACKED_DEVICE_COUNT];
bool m_rbShowTrackedDevice[MAX_TRACKED_DEVICE_COUNT];

int m_iValidPoseCount;
int m_iValidPoseCount_Last;


static vr_eye_t eyes[2];
static vr_eye_t *current_eye = NULL;
static vec3_t lastOrientation = {0, 0, 0};
static vec3_t lastAim = {0, 0, 0};

static qboolean vr_initialized = false;
static ovrMirrorTexture mirror_texture;
static ovrMirrorTextureDesc mirror_texture_desc;
static GLuint mirror_fbo = 0;
static int attempt_to_refocus_retry = 0;


// Wolfenstein 3D, DOOM and QUAKE use the same coordinate/unit system:
// 8 foot (96 inch) height wall == 64 units, 1.5 inches per pixel unit
// 1.0 pixel unit / 1.5 inch == 0.666666 pixel units per inch
static const float meters_to_units = 1.0f/(1.5f * 0.0254f);


extern cvar_t gl_farclip;
extern int glwidth, glheight;
extern void SCR_UpdateScreenContent();
extern refdef_t r_refdef;
extern cvar_t snd_device;

cvar_t vr_enabled = {"vr_enabled", "0", CVAR_NONE};
cvar_t vr_crosshair = {"vr_crosshair","1", CVAR_ARCHIVE};
cvar_t vr_crosshair_depth = {"vr_crosshair_depth","0", CVAR_ARCHIVE};
cvar_t vr_crosshair_size = {"vr_crosshair_size","3.0", CVAR_ARCHIVE};
cvar_t vr_crosshair_alpha = {"vr_crosshair_alpha","0.25", CVAR_ARCHIVE};
cvar_t vr_aimmode = {"vr_aimmode","1", CVAR_ARCHIVE};
cvar_t vr_deadzone = {"vr_deadzone","30",CVAR_ARCHIVE};
cvar_t vr_perfhud = {"vr_perfhud", "0", CVAR_ARCHIVE};


static qboolean InitOpenGLExtensions()
{
	int i;
	static qboolean extensions_initialized;

	if (extensions_initialized)
		return true;

	for( i = 0; gl_extensions[i].func; i++ ) {
		void *func = SDL_GL_GetProcAddress(gl_extensions[i].name);
		if (!func) 
			return false;

		*((void **)gl_extensions[i].func) = func;
	}

	extensions_initialized = true;
	return extensions_initialized;
}

fbo_t CreateFBO(int width, int height) {
	int i;
	fbo_t fbo;
	//ovrTextureSwapChainDesc swap_desc;
	int swap_chain_length = 0;

	fbo.size.width = width;
	fbo.size.height = height;

	glGenFramebuffersEXT(1, &fbo.framebuffer);

	glGenTextures(1, &fbo.depth_texture);
	glBindTexture(GL_TEXTURE_2D,  fbo.depth_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);

	
	//swap_desc.Type = ovrTexture_2D;
	//swap_desc.ArraySize = 1;
	//swap_desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
	//swap_desc.Width = width;
	//swap_desc.Height = height;
	//swap_desc.MipLevels = 1;
	//swap_desc.SampleCount = 1;
	//swap_desc.StaticImage = ovrFalse;
	//swap_desc.MiscFlags = 0;
	//swap_desc.BindFlags = 0;

	//ovr_CreateTextureSwapChainGL(session, &swap_desc, &fbo.swap_chain);	
	//ovr_GetTextureSwapChainLength(session, fbo.swap_chain, &swap_chain_length);
	glGenTextures(1, &fbo.m_nResolveTextureId);

	//for( i = 0; i < swap_chain_length; ++i ) {
	//	int swap_texture_id = 0;
	//	ovr_GetTextureSwapChainBufferGL(session, fbo.swap_chain, i, &swap_texture_id);

		glBindTexture(GL_TEXTURE_2D, fbo.m_nResolveTextureId);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	//}

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo.m_nResolveTextureId, 0);
	return fbo;
}

void DeleteFBO(fbo_t fbo) {
	glDeleteFramebuffersEXT(1, &fbo.framebuffer);
	glDeleteTextures(1, &fbo.depth_texture);
	glDeleteTextures(1, &fbo.depth_texture);

	//ovr_DestroyTextureSwapChain(session, fbo.swap_chain);
}

void QuatToYawPitchRoll(ovrQuatf q, vec3_t out) {
	float sqw = q.w*q.w;
	float sqx = q.x*q.x;
	float sqy = q.y*q.y;
	float sqz = q.z*q.z;
	float unit = sqx + sqy + sqz + sqw; // if normalised is one, otherwise is correction factor
	float test = q.x*q.y + q.z*q.w;
	if( test > 0.499*unit ) { // singularity at north pole
		out[YAW] = 2 * atan2(q.x,q.w) / M_PI_DIV_180;
		out[ROLL] = -M_PI/2 / M_PI_DIV_180 ;
		out[PITCH] = 0;
	}
	else if( test < -0.499*unit ) { // singularity at south pole
		out[YAW] = -2 * atan2(q.x,q.w) / M_PI_DIV_180;
		out[ROLL] = M_PI/2 / M_PI_DIV_180;
		out[PITCH] = 0;
	}
	else {
		out[YAW] = atan2(2*q.y*q.w-2*q.x*q.z , sqx - sqy - sqz + sqw) / M_PI_DIV_180;
		out[ROLL] = -asin(2*test/unit) / M_PI_DIV_180;
		out[PITCH] = -atan2(2*q.x*q.w-2*q.y*q.z , -sqx + sqy - sqz + sqw) / M_PI_DIV_180;
	}
}



void GetRotation(vec3_t out) {
	

		if (m_mat4HMDPose.m[0] == 1.0f)
		{
			out[YAW] = atan2f(m_mat4HMDPose.m[2], m_mat4HMDPose.m[11]);
			out[PITCH] = 0;
			out[ROLL] = 0;

		}
		else if (m_mat4HMDPose.m[0] == -1.0f)
		{
			out[YAW] = atan2f(m_mat4HMDPose.m[2], m_mat4HMDPose.m[11]);
			out[PITCH] = 0;
			out[ROLL] = 0;
		}
		else
		{

			out[YAW] = atan2(-m_mat4HMDPose.m[8], m_mat4HMDPose.m[0]);
			out[PITCH] = asin(m_mat4HMDPose.m[4]);
			out[ROLL] = atan2(-m_mat4HMDPose.m[6], m_mat4HMDPose.m[5]);
		}

}

void Vec3RotateZ(vec3_t in, float angle, vec3_t out) {
	out[0] = in[0]*cos(angle) - in[1]*sin(angle);
	out[1] = in[0]*sin(angle) + in[1]*cos(angle);
	out[2] = in[2];
}

ovrMatrix4f TransposeMatrix(ovrMatrix4f in) {
	ovrMatrix4f out;
	int y, x;
	for( y = 0; y < 4; y++ )
		for( x = 0; x < 4; x++ )
			out.M[x][y] = in.M[y][x];

	return out;
}


// ----------------------------------------------------------------------------
// Callbacks for cvars

static void VR_Enabled_f (cvar_t *var)
{
	VR_Disable();

	if (!vr_enabled.value) 
		return;

	if( !VR_Enable() )
		Cvar_SetValueQuick(&vr_enabled,0);
}



static void VR_Deadzone_f (cvar_t *var)
{
	// clamp the mouse to a max of 0 - 70 degrees
	float deadzone = CLAMP (0.0f, vr_deadzone.value, 70.0f);
	if (deadzone != vr_deadzone.value)
		Cvar_SetValueQuick(&vr_deadzone, deadzone);
}



static void VR_Perfhud_f (cvar_t *var)
{
	if (vr_initialized)
	{
		ovr_SetInt(session, OVR_PERF_HUD_MODE, (int)vr_perfhud.value);
	}
}



// ----------------------------------------------------------------------------
// Public vars and functions

void VR_Init()
{
	// This is only called once at game start
	Cvar_RegisterVariable (&vr_enabled);
	Cvar_SetCallback (&vr_enabled, VR_Enabled_f);
	Cvar_RegisterVariable (&vr_crosshair);
	Cvar_RegisterVariable (&vr_crosshair_depth);
	Cvar_RegisterVariable (&vr_crosshair_size);
	Cvar_RegisterVariable (&vr_crosshair_alpha);
	Cvar_RegisterVariable (&vr_aimmode);
	Cvar_RegisterVariable (&vr_deadzone);
	Cvar_SetCallback (&vr_deadzone, VR_Deadzone_f);
	Cvar_RegisterVariable (&vr_perfhud);
	Cvar_SetCallback (&vr_perfhud, VR_Perfhud_f);

	VR_Menu_Init();
// Set the cvar if invoked from a command line parameter
	
	{
		int i = COM_CheckParm("-vr");
		if ( i && i < com_argc - 1 ) {
			Cvar_SetQuick( &vr_enabled, "1" );
		}
	}
}



//-----------------------------------------------------------------------------
// Purpose: Helper to get a string from a tracked device property. Freeing the returned char* is on the caller.
//-----------------------------------------------------------------------------
char* GetTrackedDeviceString(VR_IVRSystem* pHmd, TrackedDeviceIndex_t unDevice, TrackedDeviceProperty prop, TrackedPropertyError *peError)
{
	uint32_t unRequiredBufferLen = pHmd->GetStringTrackedDeviceProperty(unDevice, prop, NULL, 0, peError);
	if (unRequiredBufferLen == 0)
		return "";

	char *pchBuffer = malloc(unRequiredBufferLen);
	unRequiredBufferLen = pHmd->GetStringTrackedDeviceProperty(unDevice, prop, pchBuffer, unRequiredBufferLen, peError);
	return pchBuffer;
}

qboolean VR_Enable()
{
	int i;
	//static ovrGraphicsLuid luid;
	int mirror_texture_id = 0;
	UINT ovr_audio_id;


	// Loading the SteamVR Runtime
	EVRInitError eError = EVRInitError_VRInitError_None;
	// VR_Init has been inlined in openvr.h to help with versioning issues.
	// The capi does not currently have that implemented, but you can follow
	// what openvr.h does and do something similar.
	// https://steamcommunity.com/app/358720/discussions/0/405692758722144628/
	VR_InitInternal(&eError, EVRApplicationType_VRApplication_Scene);
	
	if (eError != EVRInitError_VRInitError_None)
	{
		m_pHMD = NULL;
		char buf[1024];
		sprintf_s(buf, sizeof(buf), "Unable to init VR runtime: %s", VR_GetVRInitErrorAsEnglishDescription(eError));
		//SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VR_Init Failed", buf, NULL);
		return false;
	}	

	// VR_GetGenericInterface returns a pointer to a class, and we could get its vtable easily (just cast the returned
	// instance to (VR_IVRSystem**), dereference once and use). But the problem is in passing "this" and arguments to
	// mirror the used C++ calling convention, which would be prone to breaking and non-portable.
	// Better use the undocumented method of passing version prepended by "FnTable:".
	char tableName[128];
	sprintf_s(tableName, sizeof(tableName), "%s%s", VR_IVRFnTable_Prefix, IVRSystem_Version);
	m_pHMD = (VR_IVRSystem*)VR_GetGenericInterface(tableName, &eError);
	//initialize the compositor
	sprintf_s(tableName, sizeof(tableName), "%s%s", VR_IVRFnTable_Prefix, IVRCompositor_Version);
	m_pCompositor = (VR_IVRCompositor*)VR_GetGenericInterface(tableName, &eError);


	if( !InitOpenGLExtensions() ) {
		Con_Printf("Failed to initialize OpenGL extensions");
		return false;
	}
	

	mirror_texture_desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
	mirror_texture_desc.Width = glwidth;
	mirror_texture_desc.Height = glheight;

  //	ovr_CreateMirrorTextureGL(session, &mirror_texture_desc, &mirror_texture);
	glGenFramebuffersEXT(1, &mirror_fbo);
	glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, mirror_fbo);

	//ovr_GetMirrorTextureBufferGL(session, mirror_texture, &mirror_texture_id);
	glFramebufferTexture2DEXT(GL_READ_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, mirror_texture_id, 0);
	glFramebufferRenderbufferEXT(GL_READ_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, 0);
	glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, 0);
	HmdMatrix34_t matEye;

	//hmd = ovr_GetHmdDesc(session);
	float upTan, downTan, leftTan, rightTan;

	for( i = 0; i < 2; i++ ) {
		//ovrSizei size = ovr_GetFovTextureSize(session, (ovrEyeType)i, hmd.DefaultEyeFov[i], 1.0f);
		m_pHMD->GetRecommendedRenderTargetSize(&m_nRenderWidth, &m_nRenderHeight);


		//Get the raw projection stuff for each eye
		if (i == 0){
			m_pHMD->GetProjectionRaw(EVREye_Eye_Left, &leftTan, &rightTan, &upTan, &downTan);
			matEye = m_pHMD->GetEyeToHeadTransform(EVREye_Eye_Left);
		}
		else {
			m_pHMD->GetProjectionRaw(EVREye_Eye_Right, &leftTan, &rightTan, &upTan, &downTan);
			matEye = m_pHMD->GetEyeToHeadTransform(EVREye_Eye_Right);
		}

		eyes[i].index = i;
		eyes[i].fbo = CreateFBO((int)m_nRenderWidth, (int)m_nRenderHeight);
		eyes[i].HmdToEyeOffset.x = matEye.m[0][3];
		eyes[i].HmdToEyeOffset.y = matEye.m[1][3];
		eyes[i].HmdToEyeOffset.z = matEye.m[2][3];
		leftTan *= -1.0f;
		upTan *= -1.0f;
		eyes[i].fov_x = (atan(leftTan) + atan(rightTan)) / M_PI_DIV_180;
		eyes[i].fov_y = (atan(upTan) + atan(downTan)) / M_PI_DIV_180;
	}
	
	wglSwapIntervalEXT(0); // Disable V-Sync
	
	/*
	// Set the Rift as audio device
	ovr_GetAudioDeviceOutWaveId(&ovr_audio_id);
	if (ovr_audio_id != WAVE_MAPPER)
	{
		// Get the name of the device and set it as snd_device. 
		WAVEOUTCAPS caps;
		MMRESULT mmr = waveOutGetDevCaps(ovr_audio_id, &caps, sizeof(caps));
		if (mmr == MMSYSERR_NOERROR)
		{
#if SDL_MAJOR_VERSION >= 2
      char *name = SDL_iconv_string("UTF-8", "UTF-16LE", (char *)(caps.szPname), (SDL_wcslen((WCHAR *)caps.szPname)+1)*sizeof(WCHAR));
#else
      char *name = SDL_iconv_string("UTF-8", "UTF-16LE", (char *)(caps.szPname), (wcslen((WCHAR *)caps.szPname) + 1) * sizeof(WCHAR));
#endif
			Cvar_SetQuick(&snd_device, name);
		}
	}
	*/
	attempt_to_refocus_retry = 900; // Try to refocus our for the first 900 frames :/
	vr_initialized = true;
	return true;
}


void VR_Shutdown() {
	VR_Disable();
}

void VR_Disable()
{
	//int i;
	if( !vr_initialized )
		return;
	
	if (m_pHMD)
	{
		VR_ShutdownInternal();
		m_pHMD = NULL;
	}


	Cvar_SetQuick(&snd_device, "default");

	//for( i = 0; i < 2; i++ ) {
		//DeleteFBO(eyes[i].fbo);
	//}
	//ovr_DestroyMirrorTexture(session, mirror_texture);
	//ovr_Destroy(session);
	//ovr_Shutdown();

	vr_initialized = false;
}

static void RenderScreenForCurrentEye()
{
	int swap_index = 0;
	int swap_texture_id = 0;

	// Remember the current glwidht/height; we have to modify it here for each eye
	int oldglheight = glheight;
	int oldglwidth = glwidth;

	glwidth = current_eye->fbo.size.width;
	glheight = current_eye->fbo.size.height;

	
	//ovr_GetTextureSwapChainCurrentIndex(session, current_eye->fbo.swap_chain, &swap_index);
	//ovr_GetTextureSwapChainBufferGL(session, current_eye->fbo.swap_chain, swap_index, &swap_texture_id);

	// Set up current FBO
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, current_eye->fbo.framebuffer);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, current_eye->fbo.m_nResolveTextureId, 0);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, current_eye->fbo.depth_texture, 0);

	glViewport(0, 0, current_eye->fbo.size.width, current_eye->fbo.size.height);
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);


	// Draw everything
	srand((int) (cl.time * 1000)); //sync random stuff between eyes

	r_refdef.fov_x = current_eye->fov_x;
	r_refdef.fov_y = current_eye->fov_y;

	SCR_UpdateScreenContent ();
	//ovr_CommitTextureSwapChain(session, current_eye->fbo.swap_chain);
	
	
	// Reset
	glwidth = oldglwidth;
	glheight = oldglheight;
	
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, 0, 0);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, 0, 0);
}

void VR_UpdateScreenContent()
{
	int i;
	vec3_t orientation;
	ovrVector3f view_offset[2];
	ovrPosef render_pose[2];

	double ftiming, pose_time;
	ovrTrackingState hmdState;

	ovrViewScaleDesc viewScaleDesc;
	ovrLayerEyeFov ld;
	ovrLayerHeader* layers;
	GLint w, h;
	
	// Last chance to enable VR Mode - we get here when the game already start up with vr_enabled 1
	// If enabling fails, unset the cvar and return.
	if( !vr_initialized && !VR_Enable() ) {
		Cvar_Set ("vr_enabled", "0");
		return;
	}

	w = mirror_texture_desc.Width;
	h = mirror_texture_desc.Height;

	// Get current orientation of the HMD
	//ftiming = ovr_GetPredictedDisplayTime(session, 0);
	//pose_time = ovr_GetTimeInSeconds();
	//hmdState = ovr_GetTrackingState(session, ftiming, false);
	
	//Update Vive pose
	UpdateHMDMatrixPose();


	// Calculate HMD angles and blend with input angles based on current aim mode
	GetRotation(orientation);

	switch( (int)vr_aimmode.value )
	{
		// 1: (Default) Head Aiming; View YAW is mouse+head, PITCH is head
		default:
		case VR_AIMMODE_HEAD_MYAW:
			cl.viewangles[PITCH] = cl.aimangles[PITCH] = orientation[PITCH];
			cl.aimangles[YAW] = cl.viewangles[YAW] = cl.aimangles[YAW] + orientation[YAW] - lastOrientation[YAW];
			break;
		
		// 2: Head Aiming; View YAW and PITCH is mouse+head (this is stupid)
		case VR_AIMMODE_HEAD_MYAW_MPITCH:
			cl.viewangles[PITCH] = cl.aimangles[PITCH] = cl.aimangles[PITCH] + orientation[PITCH] - lastOrientation[PITCH];
			cl.aimangles[YAW] = cl.viewangles[YAW] = cl.aimangles[YAW] + orientation[YAW] - lastOrientation[YAW];
			break;
		
		// 3: Mouse Aiming; View YAW is mouse+head, PITCH is head
		case VR_AIMMODE_MOUSE_MYAW:
			cl.viewangles[PITCH] = orientation[PITCH];
			cl.viewangles[YAW]   = cl.aimangles[YAW] + orientation[YAW];
			break;
		
		// 4: Mouse Aiming; View YAW and PITCH is mouse+head
		case VR_AIMMODE_MOUSE_MYAW_MPITCH:
			cl.viewangles[PITCH] = cl.aimangles[PITCH] + orientation[PITCH];
			cl.viewangles[YAW]   = cl.aimangles[YAW] + orientation[YAW];
			break;
		
		case VR_AIMMODE_BLENDED:
		case VR_AIMMODE_BLENDED_NOPITCH:
			{
				float diffHMDYaw = orientation[YAW] - lastOrientation[YAW];
				float diffHMDPitch = orientation[PITCH] - lastOrientation[PITCH];
				float diffAimYaw = cl.aimangles[YAW] - lastAim[YAW];
				float diffYaw;

				// find new view position based on orientation delta
				cl.viewangles[YAW] += diffHMDYaw;

				// find difference between view and aim yaw
				diffYaw = cl.viewangles[YAW] - cl.aimangles[YAW];

				if (abs(diffYaw) > vr_deadzone.value / 2.0f)
				{
					// apply the difference from each set of angles to the other
					cl.aimangles[YAW] += diffHMDYaw;
					cl.viewangles[YAW] += diffAimYaw;
				}
				if ( (int)vr_aimmode.value == VR_AIMMODE_BLENDED ) {
					cl.aimangles[PITCH] += diffHMDPitch;
				}
				cl.viewangles[PITCH]  = orientation[PITCH];
			}
			break;
	}
	cl.viewangles[ROLL]  = orientation[ROLL];

	VectorCopy (orientation, lastOrientation);
	VectorCopy (cl.aimangles, lastAim);
	
	VectorCopy (cl.viewangles, r_refdef.viewangles);
	VectorCopy (cl.aimangles, r_refdef.aimangles);


	// Calculate eye poses
	view_offset[0].x = eyes[0].HmdToEyeOffset.x;
	view_offset[0].y = eyes[0].HmdToEyeOffset.y;
	view_offset[0].z = eyes[0].HmdToEyeOffset.z;

	view_offset[1].x = eyes[1].HmdToEyeOffset.x;
	view_offset[1].y = eyes[1].HmdToEyeOffset.y;
	view_offset[1].z = eyes[1].HmdToEyeOffset.z;


	//Get camera poses
	SetupCameras();
	//ovr_CalcEyePoses(hmdState.HeadPose.ThePose, view_offset, render_pose);
	eyes[0].pose = Matrix4_multiply_Matrix4(&m_mat4eyePosLeft, &m_mat4HMDPose);
	eyes[0].proj = m_mat4ProjectionLeft;
	eyes[1].pose = Matrix4_multiply_Matrix4(&m_mat4eyePosRight, &m_mat4HMDPose);
	eyes[1].proj = m_mat4ProjectionRight;



	// Render the scene for each eye into their FBOs
	for( i = 0; i < 2; i++ ) {
		current_eye = &eyes[i];
		RenderScreenForCurrentEye();
	}
	

	// Submit the FBOs to OVR
/*	viewScaleDesc.HmdSpaceToWorldScaleInMeters = meters_to_units;
	viewScaleDesc.HmdToEyeOffset[0] = view_offset[0];
	viewScaleDesc.HmdToEyeOffset[1] = view_offset[1];

	ld.Header.Type = ovrLayerType_EyeFov;
	ld.Header.Flags = ovrLayerFlag_TextureOriginAtBottomLeft;

	for( i = 0; i < 2; i++ ) {
		ld.ColorTexture[i] = eyes[i].fbo.swap_chain;
		ld.Viewport[i].Pos.x = 0;
		ld.Viewport[i].Pos.y = 0;
		ld.Viewport[i].Size.w = eyes[i].fbo.size.width;
		ld.Viewport[i].Size.h = eyes[i].fbo.size.height;
		ld.Fov[i] = hmd.DefaultEyeFov[i];
		ld.RenderPose[i] = eyes[i].pose;
		ld.SensorSampleTime = pose_time;
	}

	layers = &ld.Header;
	ovr_SubmitFrame(session, 0, &viewScaleDesc, &layers, 1);

	*/

	//submit the rendering stuffffffffffffffff
	Texture_t LeftEyeTexture = { (void*) eyes[0].fbo.m_nResolveTextureId, ETextureType_TextureType_OpenGL, EColorSpace_ColorSpace_Gamma };
	Texture_t RightEyeTexture = { (void*) eyes[1].fbo.m_nResolveTextureId, ETextureType_TextureType_OpenGL, EColorSpace_ColorSpace_Gamma };
	m_pCompositor->Submit(EVREye_Eye_Left, &LeftEyeTexture, NULL, EVRSubmitFlags_Submit_Default);
	m_pCompositor->Submit(EVREye_Eye_Right, &RightEyeTexture, NULL, EVRSubmitFlags_Submit_Default);
	glFinish();


	SDL_GL_SwapWindow(m_pCompositor);


	glFlush();
	glFinish();

	// Blit mirror texture to backbuffer
	glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, mirror_fbo);
	glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, 0);
	glBlitFramebufferEXT(0, h, w, 0, 0, 0, w, h,GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, 0);


	// OVR steals the mouse focus when fading in our window. As a stupid workaround, we simply
	// refocus the window each frame, for the first 900 frames.
	if (attempt_to_refocus_retry > 0)
	{
		VID_Refocus();
		attempt_to_refocus_retry--;
	}
}

void VR_SetMatrices() {
	vec3_t temp, orientation, position;
	Matrix4 projection;

	// Calculat HMD projection matrix and view offset position
	//projection = current_eye->proj;// TransposeMatrix(ovrMatrix4f_Projection(hmd.DefaultEyeFov[current_eye->index], 4, gl_farclip.value, ovrProjection_None));
	projection = Matrix4_multiply_Matrix4(&current_eye->proj, &current_eye->pose);
	// We need to scale the view offset position to quake units and rotate it by the current input angles (viewangle - eye orientation)
	GetRotation(orientation);
	temp[0] = -current_eye->pose.m[14] * meters_to_units;
	temp[1] = -current_eye->pose.m[12] * meters_to_units;
	temp[2] = current_eye->pose.m[13] * meters_to_units;
	Vec3RotateZ(temp, (r_refdef.viewangles[YAW]-orientation[YAW])*M_PI_DIV_180, position);


	// Set OpenGL projection and view matrices
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf((GLfloat*) projection.m);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity ();
	
	glRotatef (-90,  1, 0, 0); // put Z going up
	glRotatef (90,  0, 0, 1); // put Z going up
		
	glRotatef (-r_refdef.viewangles[PITCH],  0, 1, 0);
	glRotatef (-r_refdef.viewangles[ROLL],  1, 0, 0);
	glRotatef (-r_refdef.viewangles[YAW],  0, 0, 1);
	
	glTranslatef (-r_refdef.vieworg[0] -position[0],  -r_refdef.vieworg[1]-position[1],  -r_refdef.vieworg[2]-position[2]);
}

void VR_AddOrientationToViewAngles(vec3_t angles)
{
	vec3_t orientation;
	//	QuatToYawPitchRoll(current_eye->pose.Orientation, orientation);
	GetRotation(orientation);

	angles[PITCH] = angles[PITCH] + orientation[PITCH]; 
	angles[YAW] = angles[YAW] + orientation[YAW]; 
	angles[ROLL] = orientation[ROLL];
}

void VR_ShowCrosshair ()
{
	vec3_t forward, up, right;
	vec3_t start, end, impact;
	float size, alpha;

	if( (sv_player && (int)(sv_player->v.weapon) == IT_AXE) )
		return;

	size = CLAMP (0.0, vr_crosshair_size.value, 32.0);
	alpha = CLAMP (0.0, vr_crosshair_alpha.value, 1.0);

	if ( size <= 0 || alpha <= 0 )
		return;

	// setup gl
	glDisable (GL_DEPTH_TEST);
	glPolygonMode (GL_FRONT_AND_BACK, GL_LINE);
	GL_PolygonOffset (OFFSET_SHOWTRIS);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable (GL_TEXTURE_2D);
	glDisable (GL_CULL_FACE);

	// calc the line and draw
	VectorCopy (cl.viewent.origin, start);
	start[2] -= cl.viewheight - 10;
	AngleVectors (cl.aimangles, forward, right, up);

	switch((int) vr_crosshair.value)
	{	
		default:
		case VR_CROSSHAIR_POINT:
			if (vr_crosshair_depth.value <= 0) {
				 // trace to first wall
				VectorMA (start, 4096, forward, end);
				TraceLine (start, end, impact);
			} else {
				// fix crosshair to specific depth
				VectorMA (start, vr_crosshair_depth.value * meters_to_units, forward, impact);
			}

			glEnable(GL_POINT_SMOOTH);
			glColor4f (1, 0, 0, alpha);
			glPointSize( size * glwidth / vid.width );

			glBegin(GL_POINTS);
			glVertex3f (impact[0], impact[1], impact[2]);
			glEnd();
			glDisable(GL_POINT_SMOOTH);
			break;

		case VR_CROSSHAIR_LINE:
			// trace to first entity
			VectorMA (start, 4096, forward, end);
			TraceLineToEntity (start, end, impact, sv_player);

			glColor4f (1, 0, 0, alpha);
			glLineWidth( size * glwidth / vid.width );
			glBegin (GL_LINES);
			glVertex3f (start[0], start[1], start[2]);
			glVertex3f (impact[0], impact[1], impact[2]);
			glEnd ();
			break;
	}

	// cleanup gl
	glColor3f (1,1,1);
	glEnable (GL_TEXTURE_2D);
	glEnable (GL_CULL_FACE);
	glDisable(GL_BLEND);
	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	GL_PolygonOffset (OFFSET_NONE);
	glEnable (GL_DEPTH_TEST);
}

void VR_Draw2D()
{
	qboolean draw_sbar = false;
	vec3_t menu_angles, forward, right, up, target;
	float scale_hud = 0.13;

	int oldglwidth = glwidth, 
		oldglheight = glheight,
		oldconwidth = vid.conwidth,
		oldconheight = vid.conheight;

	glwidth = 320;
	glheight = 200;
	
	vid.conwidth = 320;
	vid.conheight = 200;

	// draw 2d elements 1m from the users face, centered
	glPushMatrix();
	glDisable (GL_DEPTH_TEST); // prevents drawing sprites on sprites from interferring with one another
	glEnable (GL_BLEND);

	VectorCopy(r_refdef.aimangles, menu_angles)

	if (vr_aimmode.value == VR_AIMMODE_HEAD_MYAW || vr_aimmode.value == VR_AIMMODE_HEAD_MYAW_MPITCH)
		menu_angles[PITCH] = 0;

	AngleVectors (menu_angles, forward, right, up);

	VectorMA (r_refdef.vieworg, 48, forward, target);

	glTranslatef (target[0],  target[1],  target[2]);
	glRotatef(menu_angles[YAW] - 90, 0, 0, 1); // rotate around z
	glRotatef(90 + menu_angles[PITCH], -1, 0, 0); // keep bar at constant angled pitch towards user
	glTranslatef (-(320.0 * scale_hud / 2), -(200.0 * scale_hud / 2), 0); // center the status bar
	glScalef(scale_hud, scale_hud, scale_hud);


	if (scr_drawdialog) //new game confirm
	{
		if (con_forcedup)
			Draw_ConsoleBackground ();
		else
			draw_sbar = true; //Sbar_Draw ();
		Draw_FadeScreen ();
		SCR_DrawNotifyString ();
	}
	else if (scr_drawloading) //loading
	{
		SCR_DrawLoading ();
		draw_sbar = true; //Sbar_Draw ();
	}
	else if (cl.intermission == 1 && key_dest == key_game) //end of level
	{
		Sbar_IntermissionOverlay ();
	}
	else if (cl.intermission == 2 && key_dest == key_game) //end of episode
	{
		Sbar_FinaleOverlay ();
		SCR_CheckDrawCenterString ();
	}
	else
	{
		//SCR_DrawCrosshair (); //johnfitz
		SCR_DrawRam ();
		SCR_DrawNet ();
		SCR_DrawTurtle ();
		SCR_DrawPause ();
		SCR_CheckDrawCenterString ();
		draw_sbar = true; //Sbar_Draw ();
		SCR_DrawDevStats (); //johnfitz
		SCR_DrawFPS (); //johnfitz
		SCR_DrawClock (); //johnfitz
		SCR_DrawConsole ();
		M_Draw ();
	}

	glDisable (GL_BLEND);
	glEnable (GL_DEPTH_TEST);
	glPopMatrix();

	if(draw_sbar)
		VR_DrawSbar();

	glwidth = oldglwidth;
	glheight = oldglheight;
	vid.conwidth = oldconwidth;
	vid.conheight =	oldconheight;
}

void VR_DrawSbar()
{	
	vec3_t sbar_angles, forward, right, up, target;
	float scale_hud = 0.025;

	glPushMatrix();
	glDisable (GL_DEPTH_TEST); // prevents drawing sprites on sprites from interferring with one another

	VectorCopy(cl.aimangles, sbar_angles)

	if (vr_aimmode.value == VR_AIMMODE_HEAD_MYAW || vr_aimmode.value == VR_AIMMODE_HEAD_MYAW_MPITCH)
		sbar_angles[PITCH] = 0;

	AngleVectors (sbar_angles, forward, right, up);

	VectorMA (cl.viewent.origin, 1.0, forward, target);

	glTranslatef (target[0],  target[1],  target[2]);
	glRotatef(sbar_angles[YAW] - 90, 0, 0, 1); // rotate around z
	glRotatef(90 + 45 + sbar_angles[PITCH], -1, 0, 0); // keep bar at constant angled pitch towards user
	glTranslatef (-(320.0 * scale_hud / 2), 0, 0); // center the status bar
	glTranslatef (0,  0,  10); // move hud down a bit
	glScalef(scale_hud, scale_hud, scale_hud);

	Sbar_Draw ();

	glEnable (GL_DEPTH_TEST);
	glPopMatrix();
}

void VR_SetAngles(vec3_t angles)
{
	VectorCopy(angles,cl.aimangles);
	VectorCopy(angles,cl.viewangles);
	VectorCopy(angles,lastAim);
}

void VR_ResetOrientation()
{
	cl.aimangles[YAW] = cl.viewangles[YAW];	
	cl.aimangles[PITCH] = cl.viewangles[PITCH];
	if (vr_enabled.value) {
		ovr_RecenterTrackingOrigin(session);
		VectorCopy(cl.aimangles,lastAim);
	}
}



//OPENVR FUNCTIONS


//-----------------------------------------------------------------------------
// Purpose:Updates pose of HMD
//-----------------------------------------------------------------------------
void UpdateHMDMatrixPose()
{
	if (!m_pHMD)
		return;

	m_pCompositor->WaitGetPoses(m_rTrackedDevicePose, k_unMaxTrackedDeviceCount, NULL, 0);

	m_iValidPoseCount = 0;
	m_strPoseClasses = "";
	for (unsigned int nDevice = 0; nDevice < k_unMaxTrackedDeviceCount; ++nDevice)
	{
		if (m_rTrackedDevicePose[nDevice].bPoseIsValid)
		{
			m_iValidPoseCount++;
			m_rmat4DevicePose[nDevice] = ConvertSteamVRMatrixToMatrix4(&m_rTrackedDevicePose[nDevice].mDeviceToAbsoluteTracking);
			if (m_rDevClassChar[nDevice] == 0)
			{
				switch (m_pHMD->GetTrackedDeviceClass(nDevice))
				{
				case ETrackedDeviceClass_TrackedDeviceClass_Controller:        m_rDevClassChar[nDevice] = 'C'; break;
				case ETrackedDeviceClass_TrackedDeviceClass_HMD:               m_rDevClassChar[nDevice] = 'H'; break;
				case ETrackedDeviceClass_TrackedDeviceClass_Invalid:           m_rDevClassChar[nDevice] = 'I'; break;
				case ETrackedDeviceClass_TrackedDeviceClass_GenericTracker:    m_rDevClassChar[nDevice] = 'G'; break;
				case ETrackedDeviceClass_TrackedDeviceClass_TrackingReference: m_rDevClassChar[nDevice] = 'T'; break;
				default:                                                       m_rDevClassChar[nDevice] = '?'; break;
				}
			}
			m_strPoseClasses += m_rDevClassChar[nDevice];
		}
	}

	if (m_rTrackedDevicePose[k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
	{
		Matrix4_invert(&m_rmat4DevicePose[k_unTrackedDeviceIndex_Hmd]);
		m_mat4HMDPose = m_rmat4DevicePose[k_unTrackedDeviceIndex_Hmd];
	}
}


//-----------------------------------------------------------------------------
// Purpose: Converts a SteamVR matrix to our local matrix class
//-----------------------------------------------------------------------------
Matrix4 ConvertSteamVRMatrixToMatrix4(const HmdMatrix34_t* matPose)
{
	Matrix4 mx = {
		{
			matPose->m[0][0], matPose->m[1][0], matPose->m[2][0], 0.0,
			matPose->m[0][1], matPose->m[1][1], matPose->m[2][1], 0.0,
			matPose->m[0][2], matPose->m[1][2], matPose->m[2][2], 0.0,
			matPose->m[0][3], matPose->m[1][3], matPose->m[2][3], 1.0f
		}
	};
	return mx;
}

//-----------------------------------------------------------------------------
// Purpose: Gets a Current View Projection Matrix with respect to nEye,
//          which may be an Eye_Left or an Eye_Right.
//-----------------------------------------------------------------------------
Matrix4 GetCurrentViewProjectionMatrix(Hmd_Eye nEye)
{
	Matrix4 matMVP, matProjEyePos;
	if (nEye == EVREye_Eye_Left)
	{
		matProjEyePos = Matrix4_multiply_Matrix4(&m_mat4ProjectionLeft, &m_mat4eyePosLeft);
	}
	else if (nEye == EVREye_Eye_Right)
	{
		matProjEyePos = Matrix4_multiply_Matrix4(&m_mat4ProjectionRight, &m_mat4eyePosRight);
	}
	matMVP = Matrix4_multiply_Matrix4(&matProjEyePos, &m_mat4HMDPose);

	return matMVP;
}


//-----------------------------------------------------------------------------
// Purpose: Gets an HMDMatrixPoseEye with respect to nEye.
//-----------------------------------------------------------------------------
Matrix4 GetHMDMatrixPoseEye(Hmd_Eye nEye)
{
	if (!m_pHMD)
		return Matrix4_identity();

	HmdMatrix34_t matEyeRight = m_pHMD->GetEyeToHeadTransform(nEye);
	Matrix4 mx = {
		.m = {
		matEyeRight.m[0][0], matEyeRight.m[1][0], matEyeRight.m[2][0], 0.0,
		matEyeRight.m[0][1], matEyeRight.m[1][1], matEyeRight.m[2][1], 0.0,
		matEyeRight.m[0][2], matEyeRight.m[1][2], matEyeRight.m[2][2], 0.0,
		matEyeRight.m[0][3], matEyeRight.m[1][3], matEyeRight.m[2][3], 1.0f
	}
	};

	Matrix4_invert(&mx);
	return mx;
}

//-----------------------------------------------------------------------------
// Purpose: Gets a Matrix Projection Eye with respect to nEye.
//-----------------------------------------------------------------------------
Matrix4 GetHMDMatrixProjectionEye(Hmd_Eye nEye)
{
	if (!m_pHMD)
		return Matrix4_identity();

	HmdMatrix44_t mat = m_pHMD->GetProjectionMatrix(nEye, m_fNearClip, gl_farclip.value);

	Matrix4 mx = {
		.m = {
		mat.m[0][0], mat.m[1][0], mat.m[2][0], mat.m[3][0],
		mat.m[0][1], mat.m[1][1], mat.m[2][1], mat.m[3][1],
		mat.m[0][2], mat.m[1][2], mat.m[2][2], mat.m[3][2],
		mat.m[0][3], mat.m[1][3], mat.m[2][3], mat.m[3][3]
	}
	};
	return mx;
}


//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void SetupCameras()
{
	m_mat4ProjectionLeft = GetHMDMatrixProjectionEye(EVREye_Eye_Left);
	m_mat4ProjectionRight = GetHMDMatrixProjectionEye(EVREye_Eye_Right);
	m_mat4eyePosLeft = GetHMDMatrixPoseEye(EVREye_Eye_Left);
	m_mat4eyePosRight = GetHMDMatrixPoseEye(EVREye_Eye_Right);
}
