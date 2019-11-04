
#include <filesystem>

#include "hud.h"
#include "cl_util.h"
#include "VRInput.h"
#include "eiface.h"
#include "pm_defs.h"
#include "pm_movevars.h"

#include "VRTextureHelper.h"
#include "VRCommon.h"

#include "vr_gl.h"
#include "LodePNG/lodepng.h"

void VRTextureHelper::PreloadAllTextures(const std::filesystem::path& path)
{
	if (std::filesystem::exists(path))
	{
		for (auto& p : std::filesystem::directory_iterator(path))
		{
			if (p.is_directory())
			{
				PreloadAllTextures(p.path());
			}
			else
			{
				unsigned int dummy;
				GetTextureInternal(p.path(), dummy, dummy);
			}
		}
	}
}

void VRTextureHelper::Init()
{
	if (m_isInitialized)
		return;

	PreloadAllTextures(GetPathFor("/textures/hud"));
	//PreloadAllTextures(GetPathFor("/textures/skybox"));

	m_isInitialized = true;
}

unsigned int VRTextureHelper::GetHDGameTexture(const std::string& name)
{
	return GetTexture("game/" + name + ".png");
}

unsigned int VRTextureHelper::GetVRSkyboxTexture(const std::string& name, int index)
{
	unsigned int texture = GetTexture("skybox/" + name + m_vrMapSkyboxIndices[index] + ".png");
	if (!texture && name != "desert")
	{
		gEngfuncs.Con_DPrintf("Skybox texture %s not found, falling back to desert.\n", (name + m_vrMapSkyboxIndices[index]).data());
		texture = GetTexture("skybox/desert" + m_vrMapSkyboxIndices[index] + ".png");
	}
	return texture;
}

unsigned int VRTextureHelper::GetVRHDSkyboxTexture(const std::string& name, int index)
{
	unsigned int texture = GetTexture("skybox/hd/" + name + m_vrMapSkyboxIndices[index] + ".png");
	if (!texture)
	{
		gEngfuncs.Con_DPrintf("HD skybox texture %s not found, falling back to SD.\n", (name + m_vrMapSkyboxIndices[index]).data());
		texture = GetVRSkyboxTexture(name, index);
	}
	return texture;
}

unsigned int VRTextureHelper::GetHLSkyboxTexture(const std::string& name, int index)
{
	unsigned int texture = GetTexture("skybox/" + name + m_hlMapSkyboxIndices[index] + ".png");
	if (!texture && name != "desert")
	{
		gEngfuncs.Con_DPrintf("Skybox texture %s not found, falling back to desert.\n", (name + m_hlMapSkyboxIndices[index]).data());
		texture = GetTexture("skybox/desert" + m_hlMapSkyboxIndices[index] + ".png");
	}
	return texture;
}

unsigned int VRTextureHelper::GetHLHDSkyboxTexture(const std::string& name, int index)
{
	unsigned int texture = GetTexture("skybox/hd/" + name + m_hlMapSkyboxIndices[index] + ".png");
	if (!texture)
	{
		gEngfuncs.Con_DPrintf("HD skybox texture %s not found, falling back to SD.\n", (name + m_hlMapSkyboxIndices[index]).data());
		texture = GetHLSkyboxTexture(name, index);
	}
	return texture;
}

const char* VRTextureHelper::GetSkyboxNameFromMapName(const std::string& mapName)
{
	auto& skyboxName = m_mapSkyboxNames.find(mapName);
	if (skyboxName != m_mapSkyboxNames.end())
		return skyboxName->second.data();
	else
		return "desert";
}

const char* VRTextureHelper::GetCurrentSkyboxName()
{
	extern playermove_t* pmove;
	if (pmove != nullptr && pmove->movevars->skyName[0] != 0)
		return pmove->movevars->skyName;
	else
		return "desert";
}

unsigned int VRTextureHelper::GetTextureInternal(const std::filesystem::path& path, unsigned int& width, unsigned int& height)
{
	std::string canonicalpathstring;
	try
	{
		canonicalpathstring = std::filesystem::canonical(path).string();
	}
	catch (std::filesystem::filesystem_error e)
	{
		canonicalpathstring = std::filesystem::absolute(path).string();
	}

	if (!canonicalpathstring.empty())
	{
		auto it = m_textures.find(canonicalpathstring);
		if (it != m_textures.end())
		{
			width = it->second.width;
			height = it->second.height;
			return it->second.texnum;
		}
	}

	GLuint texture{ 0 };

	if (std::filesystem::exists(path))
	{
		std::vector<unsigned char> image;
		unsigned int error = lodepng::decode(image, width, height, path.string().data());
		if (error)
		{
			gEngfuncs.Con_DPrintf("Error (%i) trying to load texture %s: %s\n", error, path.string().data(), lodepng_error_text(error));
		}
		else if ((width & (width - 1)) || (height & (height - 1)))
		{
			gEngfuncs.Con_DPrintf("Invalid texture %s, width and height must be power of 2!\n", path.string().data());
			width = 0;
			height = 0;
			texture = 0;
		}
		else
		{
			// Now load it into OpenGL
			ClearGLErrors();
			try
			{
				TryGLCall(glActiveTexture, GL_TEXTURE0);
				TryGLCall(glGenTextures, 1, &texture);
				TryGLCall(glBindTexture, GL_TEXTURE_2D, texture);
				TryGLCall(glTexImage2D, GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data());
				TryGLCall(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				TryGLCall(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
				TryGLCall(glGenerateMipmap, GL_TEXTURE_2D);
				TryGLCall(glBindTexture, GL_TEXTURE_2D, 0);
			}
			catch (OGLErrorException e)
			{
				gEngfuncs.Con_DPrintf("Couldn't create texture %s, error: %s\n", path.string().data(), e.what());
				width = 0;
				height = 0;
				texture = 0;
			}
		}
	}
	else
	{
		gEngfuncs.Con_DPrintf("Couldn't load texture %s, it doesn't exist.\n", path.string().data());
		width = 0;
		height = 0;
		texture = 0;
	}

	m_textures[canonicalpathstring] = Texture{ texture, width, height };
	return texture;
}

unsigned int VRTextureHelper::GetTexture(const std::string& name, unsigned int& width, unsigned int& height)
{
	return GetTextureInternal(GetPathFor("/textures/" + name), width, height);
}

unsigned int VRTextureHelper::GetTexture(const std::string& name)
{
	unsigned int dummy;
	return GetTexture(name, dummy, dummy);
}

VRTextureHelper VRTextureHelper::instance{};

// from openvr docs: Order is Front, Back, Left, Right, Top, Bottom.
const std::array<std::string, 6> VRTextureHelper::m_vrMapSkyboxIndices{
	{{"ft"},
	 {"bk"},
	 {"lf"},
	 {"rt"},
	 {"up"},
	 {"dn"}} };

// order of skybox as used by Half-Life
const std::array<std::string, 6> VRTextureHelper::m_hlMapSkyboxIndices{
	{{"rt"},
	 {"bk"},
	 {"lf"},
	 {"ft"},
	 {"up"},
	 {"dn"}} };

const std::unordered_map<std::string, std::string> VRTextureHelper::m_mapSkyboxNames{
	{{"maps/c0a0.bsp", "desert"},
	 {"maps/c0a0a.bsp", "desert"},
	 {"maps/c0a0b.bsp", "2desert"},
	 {"maps/c0a0c.bsp", "desert"},
	 {"maps/c0a0d.bsp", "desert"},
	 {"maps/c0a0e.bsp", "desert"},
	 {"maps/c1a0.bsp", "desert"},
	 {"maps/c1a0a.bsp", "desert"},
	 {"maps/c1a0b.bsp", "desert"},
	 {"maps/c1a0c.bsp", "desert"},
	 {"maps/c1a0d.bsp", "desert"},
	 {"maps/c1a0e.bsp", "xen9"},
	 {"maps/c1a1.bsp", "desert"},
	 {"maps/c1a1a.bsp", "desert"},
	 {"maps/c1a1b.bsp", "desert"},
	 {"maps/c1a1c.bsp", "desert"},
	 {"maps/c1a1d.bsp", "desert"},
	 {"maps/c1a1f.bsp", "desert"},
	 {"maps/c1a2.bsp", "desert"},
	 {"maps/c1a2a.bsp", "desert"},
	 {"maps/c1a2b.bsp", "desert"},
	 {"maps/c1a2c.bsp", "desert"},
	 {"maps/c1a2d.bsp", "desert"},
	 {"maps/c1a3.bsp", "desert"},
	 {"maps/c1a3a.bsp", "desert"},
	 {"maps/c1a3b.bsp", "dusk"},
	 {"maps/c1a3c.bsp", "dusk"},
	 {"maps/c1a3d.bsp", "desert"},
	 {"maps/c1a4.bsp", "desert"},
	 {"maps/c1a4b.bsp", "desert"},
	 {"maps/c1a4d.bsp", "desert"},
	 {"maps/c1a4e.bsp", "desert"},
	 {"maps/c1a4f.bsp", "desert"},
	 {"maps/c1a4g.bsp", "desert"},
	 {"maps/c1a4i.bsp", "desert"},
	 {"maps/c1a4j.bsp", "desert"},
	 {"maps/c1a4k.bsp", "desert"},
	 {"maps/c2a1.bsp", "desert"},
	 {"maps/c2a1a.bsp", "desert"},
	 {"maps/c2a1b.bsp", "desert"},
	 {"maps/c2a2.bsp", "night"},
	 {"maps/c2a2a.bsp", "day"},
	 {"maps/c2a2b1.bsp", "desert"},
	 {"maps/c2a2b2.bsp", "desert"},
	 {"maps/c2a2c.bsp", "desert"},
	 {"maps/c2a2d.bsp", "desert"},
	 {"maps/c2a2e.bsp", "night"},
	 {"maps/c2a2f.bsp", "desert"},
	 {"maps/c2a2g.bsp", "night"},
	 {"maps/c2a2h.bsp", "night"},
	 {"maps/c2a3.bsp", "desert"},
	 {"maps/c2a3a.bsp", "desert"},
	 {"maps/c2a3b.bsp", "desert"},
	 {"maps/c2a3c.bsp", "dawn"},
	 {"maps/c2a3d.bsp", "2desert"},
	 {"maps/c2a3e.bsp", "desert"},
	 {"maps/c2a4.bsp", "morning"},
	 {"maps/c2a4a.bsp", "desert"},
	 {"maps/c2a4b.bsp", "desert"},
	 {"maps/c2a4c.bsp", "desert"},
	 {"maps/c2a4d.bsp", "desert"},
	 {"maps/c2a4e.bsp", "desert"},
	 {"maps/c2a4f.bsp", "desert"},
	 {"maps/c2a4g.bsp", "desert"},
	 {"maps/c2a5.bsp", "desert"},
	 {"maps/c2a5a.bsp", "cliff"},
	 {"maps/c2a5b.bsp", "desert"},
	 {"maps/c2a5c.bsp", "desert"},
	 {"maps/c2a5d.bsp", "desert"},
	 {"maps/c2a5e.bsp", "desert"},
	 {"maps/c2a5f.bsp", "desert"},
	 {"maps/c2a5g.bsp", "desert"},
	 {"maps/c2a5w.bsp", "desert"},
	 {"maps/c2a5x.bsp", "desert"},
	 {"maps/c3a1.bsp", "desert"},
	 {"maps/c3a1a.bsp", "desert"},
	 {"maps/c3a1b.bsp", "desert"},
	 {"maps/c3a2.bsp", "desert"},
	 {"maps/c3a2a.bsp", "desert"},
	 {"maps/c3a2b.bsp", "desert"},
	 {"maps/c3a2c.bsp", "desert"},
	 {"maps/c3a2d.bsp", "desert"},
	 {"maps/c3a2e.bsp", "desert"},
	 {"maps/c3a2f.bsp", "desert"},
	 {"maps/c4a1.bsp", "neb7"},
	 {"maps/c4a1a.bsp", "neb7"},
	 {"maps/c4a1b.bsp", "alien1"},
	 {"maps/c4a1c.bsp", "xen8"},
	 {"maps/c4a1d.bsp", "xen8"},
	 {"maps/c4a1e.bsp", "xen8"},
	 {"maps/c4a1f.bsp", "black"},
	 {"maps/c4a2.bsp", "neb6"},
	 {"maps/c4a2a.bsp", "neb6"},
	 {"maps/c4a2b.bsp", "neb6"},
	 {"maps/c4a3.bsp", "black"},
	 {"maps/c5a1.bsp", "xen10"}} };
