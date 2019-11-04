
#include <chrono>
#include <unordered_set>

#include "VRTextureHelper.h"

#include "Matrices.h"

#include "hud.h"
#include "cl_util.h"
#include "cvardef.h"
#include "usercmd.h"
#include "const.h"

#include "entity_state.h"
#include "cl_entity.h"
#include "ref_params.h"
#include "in_defs.h"  // PITCH YAW ROLL
#include "pm_movevars.h"
#include "pm_shared.h"
#include "pm_defs.h"
#include "event_api.h"
#include "pmtrace.h"
#include "screenfade.h"
#include "shake.h"
#include "hltv.h"
#include "keydefs.h"

#include "event_api.h"
#include "triangleapi.h"
#include "r_efx.h"
#include "r_studioint.h"

#include "com_weapons.h"

#include "VRRenderer.h"
#include "VRHelper.h"
#include "VRInput.h"
#include "VRSettings.h"

#define HARDWARE_MODE
#include "com_model.h"

#include "vr_gl.h"


extern globalvars_t* gpGlobals;

VRRenderer gVRRenderer;


VRRenderer::VRRenderer()
{
	vrHelper = new VRHelper();
}

VRRenderer::~VRRenderer()
{
	delete vrHelper;
	vrHelper = nullptr;
}

void VRRenderer::Init()
{
	vrHelper->Init();

	extern void InstallModExtraDataCacheHook();
	InstallModExtraDataCacheHook();
}

void VRRenderer::VidInit()
{
}

void VRRenderer::Frame(double time)
{
	if (m_isVeryFirstFrameEver)
	{
		g_vrSettings.InitialUpdateCVARSFromJson();
		m_isVeryFirstFrameEver = false;
	}

	g_vrSettings.CheckCVARsForChanges();

	UpdateGameRenderState();

	if (m_isInMenu || !IsInGame())
	{
		g_vrInput.ShowHLMenu();
		vrHelper->PollEvents(false, m_isInMenu);
	}
	else
	{
		g_vrInput.HideHLMenu();
	}

	if (!IsInGame() || (m_isInMenu && m_wasMenuJustRendered))
	{
		CaptureCurrentScreenToTexture(vrHelper->GetVRGLMenuTexture());
		m_wasMenuJustRendered = false;
	}

	if (IsDeadInGame())
	{
		extern void ClearInputForDeath();
		ClearInputForDeath();

		extern playermove_t* pmove;
		if (pmove != nullptr)
		{
			pmove->oldbuttons = 0;
			pmove->cmd.buttons = 0;
			pmove->cmd.buttons_ex = 0;
		}

		gHUD.m_vrGrabbedLadderEntIndex = 0;
	}

	if (!IsInGame())
	{
		gHUD.m_vrGrabbedLadderEntIndex = 0;
	}
}

void VRRenderer::UpdateGameRenderState()
{
	m_isInGame = m_CalcRefdefWasCalled;
	m_CalcRefdefWasCalled = false;

	m_isInMenu = !m_HUDRedrawWasCalled;
	m_HUDRedrawWasCalled = false;

	if (!m_isInMenu)
		m_wasMenuJustRendered = false;
}

void VRRenderer::CalcRefdef(struct ref_params_s* pparams)
{
	m_CalcRefdefWasCalled = true;

	if (m_isInMenu || !IsInGame())
	{
		// Unfortunately the OpenVR overlay menu ignores controller input when we render the game scene.
		// (in fact controllers themselves aren't rendered at all)
		// So when the menu is open, we hide the game (the user will see a skybox and the menu)
		pparams->nextView = 0;
		pparams->onlyClientDraw = 1;
		m_fIsOnlyClientDraw = true;

		// should never happen, but just to be sure
		if (m_displayList != 0)
		{
			glEndList();
			glDeleteLists(m_displayList, 1);
			m_displayList = 0;
		}

		return;
	}

	if (pparams->nextView == 0)
	{
		vrHelper->PollEvents(true, m_isInMenu);
		if (!vrHelper->UpdatePositions())
		{
			// No valid HMD input, render default (2D pancake mode)
			return;
		}

		VRTextureHelper::Instance().Init();
		CheckAndIfNecessaryReplaceHDTextures();
	}

	MultiPassMode multiPassMode = MultiPassMode(int(CVAR_GET_FLOAT("vr_multipass_mode")));
	bool trueDisplayListMode = multiPassMode != MultiPassMode::MIXED_DISPLAYLIST;

	if (pparams->nextView == 0)
	{
		vrHelper->PrepareVRScene(vr::EVREye::Eye_Left);

		// Record entire engine rendering in an OpenGL display list
		m_displayList = glGenLists(1);

		// For HD textures to work, we need to execute the display list immediately (GL_COMPILE_AND_EXECUTE).
		// However, GL_COMPILE_AND_EXECUTE is less performant on AMD hardware, so we still use GL_COMPILE when HD textures are not enabled.
		glNewList(m_displayList, trueDisplayListMode ? GL_COMPILE : GL_COMPILE_AND_EXECUTE);

		pparams->nextView = 1;
		m_fIsOnlyClientDraw = false;
	}
	else
	{
		glEndList();
		vrHelper->FinishVRScene(pparams->viewport[2], pparams->viewport[3]);

		// In true display list mode, we didn't execute the display list the first time (see above)
		// thus we need to draw the recorderd display list for left eye here
		if (trueDisplayListMode)
		{
			vrHelper->PrepareVRScene(vr::EVREye::Eye_Left);
			glCallList(m_displayList);
			vrHelper->FinishVRScene(pparams->viewport[2], pparams->viewport[3]);
		}

		// draw recorderd display list for right eye
		vrHelper->PrepareVRScene(vr::EVREye::Eye_Right);
		glCallList(m_displayList);
		vrHelper->FinishVRScene(pparams->viewport[2], pparams->viewport[3]);

		// delete display list (we need a new one every frame)
		glDeleteLists(m_displayList, 1);
		m_displayList = 0;

		// send images to HMD
		vrHelper->SubmitImages();

		pparams->nextView = 0;
		pparams->onlyClientDraw = 1;
		m_fIsOnlyClientDraw = true;
	}


	vrHelper->GetViewOrg(pparams->vieworg);
	vrHelper->GetViewAngles(vr::EVREye::Eye_Left, pparams->viewangles);
	gEngfuncs.SetViewAngles(pparams->viewangles);

	// Override vieworg if we have a viewentity (trigger_camera)
	if (pparams->viewentity > pparams->maxclients)
	{
		cl_entity_t* viewentity;
		viewentity = gEngfuncs.GetEntityByIndex(pparams->viewentity);
		if (viewentity)
		{
			VectorCopy(viewentity->origin, pparams->vieworg);
		}
	}
}


void VRRenderer::DrawNormal()
{
	glPushAttrib(GL_CURRENT_BIT | GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT | GL_POLYGON_BIT | GL_TEXTURE_BIT);

	glPopAttrib();
}

void VRRenderer::DrawTransparent()
{
	glPushAttrib(GL_CURRENT_BIT | GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT | GL_POLYGON_BIT | GL_TEXTURE_BIT);

	if (m_fIsOnlyClientDraw)
	{
		GLuint backgroundTexture = VRTextureHelper::Instance().GetTexture("background.png");

		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);

		glDisable(GL_CULL_FACE);
		glCullFace(GL_FRONT_AND_BACK);

		glDisable(GL_DEPTH_TEST);

		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		glLoadIdentity();

		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
		glLoadIdentity();

		glEnable(GL_TEXTURE_2D);

		bool renderUpsideDown = false;
		if (m_isInMenu)
		{
			glBindTexture(GL_TEXTURE_2D, backgroundTexture);
			m_wasMenuJustRendered = true;
		}
		else
		{
			glBindTexture(GL_TEXTURE_2D, vrHelper->GetVRTexture(vr::EVREye::Eye_Left));
			m_wasMenuJustRendered = false;
			renderUpsideDown = true;
		}

		glColor4f(1, 1, 1, 1);

		float size = 1.f;

		glBegin(GL_QUADS);
		glTexCoord2f(0, renderUpsideDown ? 0 : 1);
		glVertex3f(-size, -size, -1.f);
		glTexCoord2f(1, renderUpsideDown ? 0 : 1);
		glVertex3f(size, -size, -1.f);
		glTexCoord2f(1, renderUpsideDown ? 1 : 0);
		glVertex3f(size, size, -1.f);
		glTexCoord2f(0, renderUpsideDown ? 1 : 0);
		glVertex3f(-size, size, -1.f);
		glEnd();

		glMatrixMode(GL_PROJECTION);
		glPopMatrix();

		glMatrixMode(GL_MODELVIEW);
		glPopMatrix();

		glBindTexture(GL_TEXTURE_2D, 0);
	}
	else
	{
		DrawHDSkyBox();

		if (CVAR_GET_FLOAT("vr_debug_controllers") != 0.f)
		{
			vrHelper->TestRenderControllerPositions();
		}

		RenderHUDSprites();

		RenderScreenOverlays();

		m_wasMenuJustRendered = false;
	}

	glPopAttrib();
}

void VRRenderer::InterceptHUDRedraw(float time, int intermission)
{
	// We don't call the HUD redraw code here - instead we just store the parameters
	// HUD redraw will be called by VRRenderer::RenderHUDSprites(), which gets called by StudioModelRenderer after rendering the viewmodels (see further below)
	// We then intercept SPR_Set and SPR_DrawAdditive and render the HUD sprites in 3d space at the controller positions

	m_hudRedrawTime = time;
	m_hudRedrawIntermission = intermission;

	m_HUDRedrawWasCalled = true;
}

void VRRenderer::GetViewAngles(float* angles)
{
	vrHelper->GetViewAngles(vr::EVREye::Eye_Right, angles);
}

Vector VRRenderer::GetMovementAngles()
{
	return vrHelper->GetMovementAngles();
}

bool VRRenderer::ShouldMirrorCurrentModel(cl_entity_t* ent)
{
	if (!ent)
		return false;

	if (gHUD.GetMirroredEnt() == ent)
		return true;

	if (gEngfuncs.GetViewModel() == ent)
		return vrHelper->IsViewEntMirrored();

	return false;
}

namespace
{
	std::unordered_set<std::string> g_handmodels{
		{
			"models/v_hand_labcoat.mdl",
			"models/v_hand_hevsuit.mdl",
			"models/SD/v_hand_labcoat.mdl",
			"models/SD/v_hand_hevsuit.mdl",
		} };
}

bool VRRenderer::IsCurrentModelHandWithSkeletalData(cl_entity_t* ent, float fingerCurl[5])
{
	if (!ent || !ent->model)
		return false;

	// check if model is hand model
	if (g_handmodels.find(ent->model->name) != g_handmodels.end())
	{
		// check if model is left hand
		if (ShouldMirrorCurrentModel(ent))
		{
			if (g_vrInput.IsDragOn(vr::TrackedControllerRole_LeftHand))
				return false;

			return g_vrInput.HasSkeletalDataForHand(vr::TrackedControllerRole_LeftHand, fingerCurl);
		}
		else
		{
			if (g_vrInput.IsDragOn(vr::TrackedControllerRole_RightHand))
				return false;

			return g_vrInput.HasSkeletalDataForHand(vr::TrackedControllerRole_RightHand, fingerCurl);
		}
	}

	return false;
}

void VRRenderer::ReverseCullface()
{
	glFrontFace(GL_CW);
}

void VRRenderer::RestoreCullface()
{
	glFrontFace(GL_CCW);
}



#define SURF_NOCULL      1    // two-sided polygon (e.g. 'water4b')
#define SURF_PLANEBACK   2    // plane should be negated
#define SURF_DRAWSKY     4    // sky surface
#define SURF_WATERCSG    8    // culled by csg (was SURF_DRAWSPRITE)
#define SURF_DRAWTURB    16   // warp surface
#define SURF_DRAWTILED   32   // face without lighmap
#define SURF_CONVEYOR    64   // scrolled texture (was SURF_DRAWBACKGROUND)
#define SURF_UNDERWATER  128  // caustics
#define SURF_TRANSPARENT 256  // it's a transparent texture (was SURF_DONTWARP)

std::unordered_map<std::string, int> m_originalTextureIDs;
void VRRenderer::ReplaceAllTexturesWithHDVersion(struct cl_entity_s* ent, bool enableHD)
{
	if (ent != nullptr && ent->model != nullptr)
	{
		for (int i = 0; i < ent->model->numtextures; i++)
		{
			texture_t* texture = ent->model->textures[i];

			// Replace textures with HD versions (created by Cyril Paulus using ESRGAN)
			if (texture != nullptr && texture->name != nullptr && strcmp("sky", texture->name) != 0 && texture->name[0] != '{'  // skip transparent textures, they are broken
				)
			{
				// Backup
				if (m_originalTextureIDs.count(texture->name) == 0)
				{
					m_originalTextureIDs[texture->name] = texture->gl_texturenum;
				}

				// Replace
				if (enableHD)
				{
					unsigned int hdTex = VRTextureHelper::Instance().GetHDGameTexture(texture->name);
					if (hdTex)
					{
						texture->gl_texturenum = int(hdTex);
					}
				}
				// Restore
				else
				{
					texture->gl_texturenum = m_originalTextureIDs[texture->name];
				}
			}
		}
	}
}

void VRRenderer::CheckAndIfNecessaryReplaceHDTextures(struct cl_entity_s* map)
{
	bool replaceHDTextures = false;
	if (map->model != m_currentMapModel)
	{
		m_originalTextureIDs.clear();
		m_currentMapModel = map->model;
		replaceHDTextures = true;
		vrHelper->SetSkyboxFromMap(map->model->name);
	}
	bool hdTexturesEnabled = CVAR_GET_FLOAT("vr_hd_textures_enabled") != 0.f;
	if (hdTexturesEnabled != m_hdTexturesEnabled)
	{
		m_hdTexturesEnabled = hdTexturesEnabled;
		replaceHDTextures = true;
	}
	if (m_hdTexturesEnabled && !replaceHDTextures)
	{
		if (m_originalTextureIDs.empty())
		{
			replaceHDTextures = true;
		}
		else
		{
			// Check if map uses different textures than it should (can happen after loading the same map again)
			if (map->model != nullptr)
			{
				for (int i = 0; !replaceHDTextures && i < map->model->numtextures; i++)
				{
					texture_t* texture = map->model->textures[i];
					if (texture != nullptr && texture->name != nullptr && strcmp("sky", texture->name) != 0 && texture->name[0] != '{'  // skip transparent textures, they are broken
						)
					{
						if (m_originalTextureIDs.count(texture->name) == 0 || m_originalTextureIDs[texture->name] == texture->gl_texturenum)
						{
							replaceHDTextures = true;
							break;
						}
					}
				}
			}
		}
	}
	if (replaceHDTextures)
	{
		vrHelper->SetSkyboxFromMap(map->model->name);
		ReplaceAllTexturesWithHDVersion(map, m_hdTexturesEnabled);
	}
}

void VRRenderer::CheckAndIfNecessaryReplaceHDTextures()
{
	glEnable(GL_CULL_FACE);
	glCullFace(GL_FRONT);
	glBindTexture(GL_TEXTURE_2D, 0);
	glColor4f(0, 0, 0, 1);

	cl_entity_s* map = gEngfuncs.GetEntityByIndex(0);
	if (map != nullptr && map->model != nullptr)
	{
		CheckAndIfNecessaryReplaceHDTextures(map);
	}
	else
	{
		m_currentMapModel = nullptr;
	}
}



// for studiomodelrenderer

void VRRenderer::EnableDepthTest()
{
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDepthRange(0, 1);
}

bool VRRenderer::IsDeadInGame()
{
	return gEngfuncs.pfnGetLevelName() != 0 && gEngfuncs.pfnGetLevelName()[0] != '\0' && gEngfuncs.GetMaxClients() > 0 && CL_IsDead();
}

bool VRRenderer::IsInGame()
{
	return m_isInGame && gEngfuncs.pfnGetLevelName() != 0 && gEngfuncs.pfnGetLevelName()[0] != '\0' && gEngfuncs.GetMaxClients() > 0 && !CL_IsDead() && !gHUD.m_iIntermission;
}

cl_entity_t* VRRenderer::SaveGetLocalPlayer()
{
	if (IsInGame())
	{
		return gEngfuncs.GetLocalPlayer();
	}
	else
	{
		return nullptr;
	}
}

extern cl_entity_t* SaveGetLocalPlayer()
{
	return gVRRenderer.SaveGetLocalPlayer();
}

vr::IVRSystem* VRRenderer::GetVRSystem()
{
	return vrHelper->GetVRSystem();
}

void VRRenderer::SetViewOfs(const Vector& viewOfs)
{
	vrHelper->SetViewOfs(viewOfs);
}

void VRRenderer::DrawHDSkyBox()
{
	bool hdTexturesEnabled = CVAR_GET_FLOAT("vr_hd_textures_enabled") != 0.f;
	if (!hdTexturesEnabled)
		return;

	float size = 2048.f;
	extern playermove_t* pmove;
	if (pmove)
	{
		size = (std::max)(size, pmove->movevars->zmax * 0.5f);
	}
	float nsize = size - 8.f;

	/*
	{"rt"},
	{ "bk" },
	{ "lf" },
	{ "ft" },
	{ "up" },
	{ "dn" }
	*/

	/*
	0,1
	1,1
	1,0
	0,0
	*/

	const Vector skyboxbounds[6][4] = {
		{
			// right
			{nsize, size, -size},
			{nsize, -size, -size},
			{nsize, -size, size},
			{nsize, size, size},
		},
		{
			// back
			{-size, nsize, -size},
			{size, nsize, -size},
			{size, nsize, size},
			{-size, nsize, size},
		},
		{
			// left
			{-nsize, -size, -size},
			{-nsize, size, -size},
			{-nsize, size, size},
			{-nsize, -size, size},
		},
		{
			// front
			{size, -nsize, -size},
			{-size, -nsize, -size},
			{-size, -nsize, size},
			{size, -nsize, size},
		},
		{
			// up
			{nsize, size, nsize},
			{nsize, -size, nsize},
			{-nsize, -size, nsize},
			{-nsize, size, nsize},
		},
		{
			// down
			{-nsize, size, -nsize},
			{-nsize, -size, -nsize},
			{nsize, -size, -nsize},
			{nsize, size, -nsize},
		},
	};

	Vector skyboxorigin;
	vrHelper->GetViewOrg(skyboxorigin);

	ClearGLErrors();

	try
	{
		//TryGLCall(glDisable, GL_FOG);
		TryGLCall(glDisable, GL_BLEND);
		TryGLCall(glDisable, GL_ALPHA_TEST);
		TryGLCall(glDisable, GL_DEPTH_TEST);
		TryGLCall(glDisable, GL_CULL_FACE);
		TryGLCall(glDepthMask, GL_FALSE);
		TryGLCall(glTexEnvi, GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

		TryGLCall(glColor4f, 1.f, 1.f, 1.f, 1.f);

		TryGLCall(glEnable, GL_TEXTURE_2D);
		TryGLCall(glActiveTexture, GL_TEXTURE0);

		for (int i = 0; i < 6; i++)
		{
			TryGLCall(glBindTexture, GL_TEXTURE_2D, VRTextureHelper::Instance().GetHLHDSkyboxTexture(VRTextureHelper::Instance().GetCurrentSkyboxName(), i));

			glBegin(GL_QUADS);

			glTexCoord2f(0.f, 1.f);
			glVertex3fv(skyboxorigin + skyboxbounds[i][0]);

			glTexCoord2f(1.f, 1.f);
			glVertex3fv(skyboxorigin + skyboxbounds[i][1]);

			glTexCoord2f(1.f, 0.f);
			glVertex3fv(skyboxorigin + skyboxbounds[i][2]);

			glTexCoord2f(0.f, 0.f);
			glVertex3fv(skyboxorigin + skyboxbounds[i][3]);

			glEnd();
		}
	}
	catch (const OGLErrorException & e)
	{
		gEngfuncs.Con_DPrintf("Failed to render HD sky: %s\n", e.what());
	}
}



// For ev_common.cpp
Vector VRGlobalGetGunPosition()
{
	return gVRRenderer.GetHelper()->GetGunPosition();
}
void VRGlobalGetGunAim(Vector& forward, Vector& right, Vector& up, Vector& angles)
{
	gVRRenderer.GetHelper()->GetGunAim(forward, right, up, angles);
}
