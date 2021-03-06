/*
mapgen.cpp
Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>
*/

/*
This file is part of Freeminer.

Freeminer is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Freeminer  is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Freeminer.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <fstream>
#include "mapgen.h"
#include "voxel.h"
#include "noise.h"
#include "gamedef.h"
#include "mg_biome.h"
#include "mapblock.h"
#include "mapnode.h"
#include "map.h"
#include "content_sao.h"
#include "nodedef.h"
#include "emerge.h"
#include "content_mapnode.h" // For content_mapnode_get_new_name
#include "voxelalgorithms.h"
#include "porting.h"
#include "profiler.h"
#include "settings.h"
#include "treegen.h"
#include "serialization.h"
#include "util/serialize.h"
#include "util/numeric.h"
#include "filesys.h"
#include "log.h"

FlagDesc flagdesc_mapgen[] = {
	{"trees",    MG_TREES},
	{"caves",    MG_CAVES},
	{"dungeons", MG_DUNGEONS},
	{"flat",     MG_FLAT},
	{"light",    MG_LIGHT},
	{NULL,       0}
};

FlagDesc flagdesc_gennotify[] = {
	{"dungeon",          1 << GENNOTIFY_DUNGEON},
	{"temple",           1 << GENNOTIFY_TEMPLE},
	{"cave_begin",       1 << GENNOTIFY_CAVE_BEGIN},
	{"cave_end",         1 << GENNOTIFY_CAVE_END},
	{"large_cave_begin", 1 << GENNOTIFY_LARGECAVE_BEGIN},
	{"large_cave_end",   1 << GENNOTIFY_LARGECAVE_END},
	{"decoration",       1 << GENNOTIFY_DECORATION},
	{NULL,               0}
};


///////////////////////////////////////////////////////////////////////////////


Mapgen::Mapgen()
{
	generating    = false;
	id            = -1;
	seed          = 0;
	water_level   = 0;
	liquid_pressure = 0;
	flags         = 0;

	vm          = NULL;
	ndef        = NULL;
	heightmap   = NULL;
	biomemap    = NULL;

}


Mapgen::Mapgen(int mapgenid, MapgenParams *params, EmergeManager *emerge) :
	gennotify(emerge->gen_notify_on, &emerge->gen_notify_on_deco_ids)
{
	generating    = false;
	id            = mapgenid;
	seed          = (int)params->seed;
	water_level   = params->water_level;
	liquid_pressure = params->liquid_pressure;
	flags         = params->flags;
	csize         = v3s16(1, 1, 1) * (params->chunksize * MAP_BLOCKSIZE);

	vm        = NULL;
	ndef      = NULL;
	heightmap = NULL;
	biomemap  = NULL;
}


Mapgen::~Mapgen()
{
}


u32 Mapgen::getBlockSeed(v3s16 p, int seed)
{
	return (u32)seed   +
		p.Z * 38134234 +
		p.Y * 42123    +
		p.X * 23;
}


u32 Mapgen::getBlockSeed2(v3s16 p, int seed)
{
	u32 n = 1619 * p.X + 31337 * p.Y + 52591 * p.Z + 1013 * seed;
	n = (n >> 13) ^ n;
	return (n * (n * n * 60493 + 19990303) + 1376312589);
}


// Returns Y one under area minimum if not found
s16 Mapgen::findGroundLevelFull(v2s16 p2d)
{
	v3s16 em = vm->m_area.getExtent();
	s16 y_nodes_max = vm->m_area.MaxEdge.Y;
	s16 y_nodes_min = vm->m_area.MinEdge.Y;
	u32 i = vm->m_area.index(p2d.X, y_nodes_max, p2d.Y);
	s16 y;

	for (y = y_nodes_max; y >= y_nodes_min; y--) {
		MapNode &n = vm->m_data[i];
		if (ndef->get(n).walkable)
			break;

		vm->m_area.add_y(em, i, -1);
	}
	return (y >= y_nodes_min) ? y : y_nodes_min - 1;
}


// Returns -MAP_GENERATION_LIMIT if not found
s16 Mapgen::findGroundLevel(v2s16 p2d, s16 ymin, s16 ymax)
{
	v3s16 em = vm->m_area.getExtent();
	u32 i = vm->m_area.index(p2d.X, ymax, p2d.Y);
	s16 y;

	for (y = ymax; y >= ymin; y--) {
		MapNode &n = vm->m_data[i];
		if (ndef->get(n).walkable)
			break;

		vm->m_area.add_y(em, i, -1);
	}
	return (y >= ymin) ? y : -MAP_GENERATION_LIMIT;
}


void Mapgen::updateHeightmap(v3s16 nmin, v3s16 nmax)
{
	if (!heightmap)
		return;

	//TimeTaker t("Mapgen::updateHeightmap", NULL, PRECISION_MICRO);
	int index = 0;
	for (s16 z = nmin.Z; z <= nmax.Z; z++) {
		for (s16 x = nmin.X; x <= nmax.X; x++, index++) {
			s16 y = findGroundLevel(v2s16(x, z), nmin.Y, nmax.Y);

			heightmap[index] = y;
		}
	}
	//printf("updateHeightmap: %dus\n", t.stop());
}


void Mapgen::updateLiquid(v3POS nmin, v3POS nmax)
{
	bool isliquid, wasliquid, rare;
	v3s16 em  = vm->m_area.getExtent();
	rare = g_settings->getBool("liquid_real");
	int rarecnt = 0;

	for (s16 z = nmin.Z; z <= nmax.Z; z++) {
		for (s16 x = nmin.X; x <= nmax.X; x++) {
			wasliquid = true;

			u32 i = vm->m_area.index(x, nmax.Y, z);
			for (s16 y = nmax.Y; y >= nmin.Y; y--) {
				isliquid = ndef->get(vm->m_data[i]).isLiquid();

				// there was a change between liquid and nonliquid, add to queue. no need to add every with liquid_real
				if (isliquid != wasliquid && (!rare || !(rarecnt++ % 36))) {
						auto p = v3s16(x, y, z);
						vm->m_map->transforming_liquid_push_back(p);
					}

				wasliquid = isliquid;
				vm->m_area.add_y(em, i, -1);
			}
		}
	}
}


void Mapgen::setLighting(u8 light, v3s16 nmin, v3s16 nmax)
{
	ScopeProfiler sp(g_profiler, "EmergeThread: mapgen lighting update", SPT_AVG);
	VoxelArea a(nmin, nmax);

	for (int z = a.MinEdge.Z; z <= a.MaxEdge.Z; z++) {
		for (int y = a.MinEdge.Y; y <= a.MaxEdge.Y; y++) {
			u32 i = vm->m_area.index(a.MinEdge.X, y, z);
			for (int x = a.MinEdge.X; x <= a.MaxEdge.X; x++, i++)
				vm->m_data[i].param1 = light;
		}
	}
}


void Mapgen::lightSpread(VoxelArea &a, v3s16 p, u8 light)
{
	if (light <= 1 || !a.contains(p))
		return;

	u32 vi = vm->m_area.index(p);
	MapNode &nn = vm->m_data[vi];

	light--;
	// should probably compare masked, but doesn't seem to make a difference
	if (light <= nn.param1 || !ndef->get(nn).light_propagates)
		return;

	nn.param1 = light;

	lightSpread(a, p + v3s16(0, 0, 1), light);
	lightSpread(a, p + v3s16(0, 1, 0), light);
	lightSpread(a, p + v3s16(1, 0, 0), light);
	lightSpread(a, p - v3s16(0, 0, 1), light);
	lightSpread(a, p - v3s16(0, 1, 0), light);
	lightSpread(a, p - v3s16(1, 0, 0), light);
}


void Mapgen::calcLighting(v3s16 nmin, v3s16 nmax, v3s16 full_nmin, v3s16 full_nmax)
{
	ScopeProfiler sp(g_profiler, "EmergeThread: mapgen lighting update", SPT_AVG);
	//TimeTaker t("updateLighting");

	propagateSunlight(nmin, nmax);
	spreadLight(full_nmin, full_nmax);

	//printf("updateLighting: %dms\n", t.stop());
}



void Mapgen::calcLighting(v3s16 nmin, v3s16 nmax)
{
	ScopeProfiler sp(g_profiler, "EmergeThread: mapgen lighting update", SPT_AVG);
	//TimeTaker t("updateLighting");

	propagateSunlight(
		nmin - v3s16(1, 1, 1) * MAP_BLOCKSIZE,
		nmax + v3s16(1, 0, 1) * MAP_BLOCKSIZE);

	spreadLight(
		nmin - v3s16(1, 1, 1) * MAP_BLOCKSIZE,
		nmax + v3s16(1, 1, 1) * MAP_BLOCKSIZE);

	//printf("updateLighting: %dms\n", t.stop());
}


void Mapgen::propagateSunlight(v3s16 nmin, v3s16 nmax)
{
	//TimeTaker t("propagateSunlight");
	VoxelArea a(nmin, nmax);
	bool block_is_underground = (water_level >= nmax.Y);
	v3s16 em = vm->m_area.getExtent();

	for (int z = a.MinEdge.Z; z <= a.MaxEdge.Z; z++) {
		for (int x = a.MinEdge.X; x <= a.MaxEdge.X; x++) {
			// see if we can get a light value from the overtop
			u32 i = vm->m_area.index(x, a.MaxEdge.Y + 1, z);
			if (vm->m_data[i].getContent() == CONTENT_IGNORE) {
				if (block_is_underground)
					continue;
			} else if ((vm->m_data[i].param1 & 0x0F) != LIGHT_SUN) {
				u32 ii = 0;
				if (
				(x < a.MaxEdge.X && (ii = vm->m_area.index(x + 1, a.MaxEdge.Y + 1, z    )) &&
				(vm->m_data[ii].getContent() != CONTENT_IGNORE) &&
				((vm->m_data[ii].param1 & 0x0F) == LIGHT_SUN))||
				(x > a.MinEdge.X && (ii = vm->m_area.index(x - 1, a.MaxEdge.Y + 1, z    )) &&
				(vm->m_data[ii].getContent() != CONTENT_IGNORE) &&
				((vm->m_data[ii].param1 & 0x0F) == LIGHT_SUN))||
				(z > a.MinEdge.Z && (ii = vm->m_area.index(x    , a.MaxEdge.Y + 1, z - 1)) &&
				(vm->m_data[ii].getContent() != CONTENT_IGNORE) &&
				((vm->m_data[ii].param1 & 0x0F) == LIGHT_SUN))||
				(z < a.MaxEdge.Z && (ii = vm->m_area.index(x    , a.MaxEdge.Y + 1, z + 1)) &&
				(vm->m_data[ii].getContent() != CONTENT_IGNORE) &&
				((vm->m_data[ii].param1 & 0x0F) == LIGHT_SUN))
				) {
				} else
				continue;
			}
			vm->m_area.add_y(em, i, -1);

			for (int y = a.MaxEdge.Y; y >= a.MinEdge.Y; y--) {
				MapNode &n = vm->m_data[i];
				if (!ndef->get(n).sunlight_propagates)
					break;
				n.param1 = LIGHT_SUN;
				vm->m_area.add_y(em, i, -1);
			}
		}
	}
	//printf("propagateSunlight: %dms\n", t.stop());
}



void Mapgen::spreadLight(v3s16 nmin, v3s16 nmax)
{
	//TimeTaker t("spreadLight");
	VoxelArea a(nmin, nmax);

	for (int z = a.MinEdge.Z; z <= a.MaxEdge.Z; z++) {
		for (int y = a.MinEdge.Y; y <= a.MaxEdge.Y; y++) {
			u32 i = vm->m_area.index(a.MinEdge.X, y, z);
			for (int x = a.MinEdge.X; x <= a.MaxEdge.X; x++, i++) {
				MapNode &n = vm->m_data[i];
				if (n.getContent() == CONTENT_IGNORE ||
					!ndef->get(n).light_propagates)
					continue;

				u8 light_produced = ndef->get(n).light_source & 0x0F;
				if (light_produced)
					n.param1 = light_produced;

				u8 light = n.param1 & 0x0F;
				if (light) {
					lightSpread(a, v3s16(x,     y,     z + 1), light);
					lightSpread(a, v3s16(x,     y + 1, z    ), light);
					lightSpread(a, v3s16(x + 1, y,     z    ), light);
					lightSpread(a, v3s16(x,     y,     z - 1), light);
					lightSpread(a, v3s16(x,     y - 1, z    ), light);
					lightSpread(a, v3s16(x - 1, y,     z    ), light);
				}
			}
		}
	}

	//printf("spreadLight: %dms\n", t.stop());
}



void Mapgen::calcLightingOld(v3s16 nmin, v3s16 nmax)
{
	enum LightBank banks[2] = {LIGHTBANK_DAY, LIGHTBANK_NIGHT};
	VoxelArea a(nmin, nmax);
	bool block_is_underground = (water_level > nmax.Y);
	bool sunlight = !block_is_underground;

	ScopeProfiler sp(g_profiler, "EmergeThread: mapgen lighting update", SPT_AVG);

	for (int i = 0; i < 2; i++) {
		enum LightBank bank = banks[i];
		std::set<v3s16> light_sources;
		std::map<v3s16, u8> unlight_from;

		voxalgo::clearLightAndCollectSources(*vm, a, bank, ndef,
			light_sources, unlight_from);
		voxalgo::propagateSunlight(*vm, a, sunlight, light_sources, ndef);

		vm->unspreadLight(bank, unlight_from, light_sources, ndef);
		vm->spreadLight(bank, light_sources, ndef);
	}
}


///////////////////////////////////////////////////////////////////////////////

GenerateNotifier::GenerateNotifier()
{
	m_notify_on = 0;
}


GenerateNotifier::GenerateNotifier(u32 notify_on,
	std::set<u32> *notify_on_deco_ids)
{
	m_notify_on = notify_on;
	m_notify_on_deco_ids = notify_on_deco_ids;
}


void GenerateNotifier::setNotifyOn(u32 notify_on)
{
	m_notify_on = notify_on;
}


void GenerateNotifier::setNotifyOnDecoIds(std::set<u32> *notify_on_deco_ids)
{
	m_notify_on_deco_ids = notify_on_deco_ids;
}


bool GenerateNotifier::addEvent(GenNotifyType type, v3s16 pos, u32 id)
{
	if (!(m_notify_on & (1 << type)))
		return false;

	if (type == GENNOTIFY_DECORATION &&
		m_notify_on_deco_ids->find(id) == m_notify_on_deco_ids->end())
		return false;

	GenNotifyEvent gne;
	gne.type = type;
	gne.pos  = pos;
	gne.id   = id;
	m_notify_events.push_back(gne);

	return true;
}


void GenerateNotifier::getEvents(
	std::map<std::string, std::vector<v3s16> > &event_map,
	bool peek_events)
{
	std::list<GenNotifyEvent>::iterator it;

	for (it = m_notify_events.begin(); it != m_notify_events.end(); ++it) {
		GenNotifyEvent &gn = *it;
		std::string name = (gn.type == GENNOTIFY_DECORATION) ?
			"decoration#"+ itos(gn.id) :
			flagdesc_gennotify[gn.type].name;

		event_map[name].push_back(gn.pos);
	}

	if (!peek_events)
		m_notify_events.clear();
}


///////////////////////////////////////////////////////////////////////////////


ObjDefManager::ObjDefManager(IGameDef *gamedef, ObjDefType type)
{
	m_objtype = type;
	m_ndef = gamedef->getNodeDefManager();
}


ObjDefManager::~ObjDefManager()
{
/* fmtodo ugly double free fix
	for (size_t i = 0; i != m_objects.size(); i++)
		delete m_objects[i];
*/
}


ObjDefHandle ObjDefManager::add(ObjDef *obj)
{
	assert(obj);

	if (obj->name.length() && getByName(obj->name))
		return OBJDEF_INVALID_HANDLE;

	u32 index = addRaw(obj);
	if (index == OBJDEF_INVALID_INDEX)
		return OBJDEF_INVALID_HANDLE;

	obj->handle = createHandle(index, m_objtype, obj->uid);
	return obj->handle;
}


ObjDef *ObjDefManager::get(ObjDefHandle handle) const
{
	u32 index = validateHandle(handle);
	return (index != OBJDEF_INVALID_INDEX) ? getRaw(index) : NULL;
}


ObjDef *ObjDefManager::set(ObjDefHandle handle, ObjDef *obj)
{
	u32 index = validateHandle(handle);
	return (index != OBJDEF_INVALID_INDEX) ? setRaw(index, obj) : NULL;
}


u32 ObjDefManager::addRaw(ObjDef *obj)
{
	size_t nobjects = m_objects.size();
	if (nobjects >= OBJDEF_MAX_ITEMS)
		return -1;

	obj->index = nobjects;

	// Ensure UID is nonzero so that a valid handle == OBJDEF_INVALID_HANDLE
	// is not possible.  The slight randomness bias isn't very significant.
	obj->uid = myrand() & OBJDEF_UID_MASK;
	if (obj->uid == 0)
		obj->uid = 1;

	m_objects.push_back(obj);

	infostream << "ObjDefManager: added " << getObjectTitle()
		<< ": name=\"" << obj->name
		<< "\" index=" << obj->index
		<< " uid="     << obj->uid
		<< std::endl;

	return nobjects;
}


ObjDef *ObjDefManager::getRaw(u32 index) const
{
	return m_objects[index];
}


ObjDef *ObjDefManager::setRaw(u32 index, ObjDef *obj)
{
	ObjDef *old_obj = m_objects[index];
	m_objects[index] = obj;
	return old_obj;
}


ObjDef *ObjDefManager::getByName(const std::string &name) const
{
	for (size_t i = 0; i != m_objects.size(); i++) {
		ObjDef *obj = m_objects[i];
		if (obj && !strcasecmp(name.c_str(), obj->name.c_str()))
			return obj;
	}

	return NULL;
}


void ObjDefManager::clear()
{
	for (size_t i = 0; i != m_objects.size(); i++)
		delete m_objects[i];

	m_objects.clear();
}


u32 ObjDefManager::validateHandle(ObjDefHandle handle) const
{
	ObjDefType type;
	u32 index;
	u32 uid;

	bool is_valid =
		(handle != OBJDEF_INVALID_HANDLE)         &&
		decodeHandle(handle, &index, &type, &uid) &&
		(type == m_objtype)                       &&
		(index < m_objects.size())                &&
		(m_objects[index]->uid == uid);

	return is_valid ? index : -1;
}


ObjDefHandle ObjDefManager::createHandle(u32 index, ObjDefType type, u32 uid)
{
	ObjDefHandle handle = 0;
	set_bits(&handle, 0, 18, index);
	set_bits(&handle, 18, 6, type);
	set_bits(&handle, 24, 7, uid);

	u32 parity = calc_parity(handle);
	set_bits(&handle, 31, 1, parity);

	return handle ^ OBJDEF_HANDLE_SALT;
}


bool ObjDefManager::decodeHandle(ObjDefHandle handle, u32 *index,
	ObjDefType *type, u32 *uid)
{
	handle ^= OBJDEF_HANDLE_SALT;

	u32 parity = get_bits(handle, 31, 1);
	set_bits(&handle, 31, 1, 0);
	if (parity != calc_parity(handle))
		return false;

	*index = get_bits(handle, 0, 18);
	*type  = (ObjDefType)get_bits(handle, 18, 6);
	*uid   = get_bits(handle, 24, 7);
	return true;
}


///////////////////////////////////////////////////////////////////////////////


void MapgenParams::load(Settings &settings)
{
	std::string seed_str;
	const char *seed_name = (&settings == g_settings) ? "fixed_map_seed" : "seed";

	if (settings.getNoEx(seed_name, seed_str) && !seed_str.empty())
		seed = read_seed(seed_str.c_str());
	else
		myrand_bytes(&seed, sizeof(seed));

	settings.getNoEx("mg_name", mg_name);
	settings.getS16NoEx("water_level", water_level);
	settings.getS16NoEx("liquid_pressure", liquid_pressure);
	settings.getS16NoEx("chunksize", chunksize);
	settings.getFlagStrNoEx("mg_flags", flags, flagdesc_mapgen);
	settings.getNoiseParams("mg_biome_np_heat", np_biome_heat);
	settings.getNoiseParams("mg_biome_np_humidity", np_biome_humidity);

	delete sparams;
	sparams = EmergeManager::createMapgenParams(mg_name);
	if (sparams)
		sparams->readParams(&settings);
}


void MapgenParams::save(Settings &settings) const
{
	settings.set("mg_name", mg_name);
	settings.setU64("seed", seed);
	settings.setS16("water_level", water_level);
	settings.setS16("liquid_pressure", liquid_pressure);
	settings.setS16("chunksize", chunksize);
	settings.setFlagStr("mg_flags", flags, flagdesc_mapgen, (u32)-1);
	settings.setNoiseParams("mg_biome_np_heat", np_biome_heat);
	settings.setNoiseParams("mg_biome_np_humidity", np_biome_humidity);

	if (sparams)
		sparams->writeParams(&settings);
}

