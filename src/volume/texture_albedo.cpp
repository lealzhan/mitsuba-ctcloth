/*
    This file is part of Mitsuba, a physically based rendering system.

    Copyright (c) 2007-2010 by Wenzel Jakob and others.

    Mitsuba is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License Version 3
    as published by the Free Software Foundation.

    Mitsuba is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <mitsuba/render/volume2.h>
#include <mitsuba/core/plugin.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/fresolver.h>
#include <mitsuba/core/fstream.h>
#include <map>


MTS_NAMESPACE_BEGIN


class TextureAlbedoVolume : public VolumeDataSourceEx {
public:
	TextureAlbedoVolume(const Properties &props) : VolumeDataSourceEx(props) {
		m_textureAlbedoFile = props.getString("textureAlbedoFile", "");
	}

	TextureAlbedoVolume(Stream *stream, InstanceManager *manager)
	: VolumeDataSourceEx(stream, manager) {
        m_block = static_cast<VolumeDataSourceEx *>(manager->getInstance(stream));
        configure();
	}

	virtual ~TextureAlbedoVolume() {
	}

	void serialize(Stream *stream, InstanceManager *manager) const {
		VolumeDataSource::serialize(stream, manager);
        manager->serialize(stream, m_block.get());
	}
		
	void configure() {
        if (m_block.get() == NULL)
            Log(EError, "No embedded volume specified!");

		if (m_textureAlbedoFile != "") {
			loadTextureAlbedo(m_textureAlbedoFile);
		}

        m_stepSize = m_block->getStepSize();
		m_aabb = m_block->getAABB();

		Vector volumeExtents = m_aabb.getExtents();
		m_worldToTexture =
			Transform::scale(Vector(
				static_cast<Float>(m_reso.x) / volumeExtents.x,
				static_cast<Float>(m_reso.y) / volumeExtents.y,
				static_cast<Float>(m_reso.z) / volumeExtents.z
				))*
			Transform::translate(Vector(
				-m_aabb.min.x, -m_aabb.min.y, -m_aabb.min.z));

		std::ostringstream oss;
        oss << "Data AABB: " << m_aabb.toString() << "\nAABB: " << m_aabb.toString() << '\n';
		oss << "Step size = " << m_stepSize << "\n";
		Log(EDebug, oss.str().c_str());
	}

	void loadTextureAlbedo(const std::string &filename) {
		fs::path resolved = Thread::getThread()->getFileResolver()->resolve(filename);
		ref<FileStream> stream = new FileStream(resolved, FileStream::EReadOnly);
		stream->setByteOrder(Stream::ELittleEndian);

		m_reso = Vector3i(stream);
		Log(EDebug, "%%%%------------ albedo map info: ------------------\n");
		Log(EDebug, "m_reso: %d, %d, %d\n", m_reso.x, m_reso.y, m_reso.z);

		m_nSpectrumItems = stream->readInt();
		for (size_t i = 0; i < m_nSpectrumItems; ++i) {
			int v = stream->readInt();
			float r = stream->readFloat();
			float g = stream->readFloat();
			float b = stream->readFloat();
			Spectrum s;
			s.fromLinearRGB(r, g, b);
			m_spectrumMap.insert(spectrum_map_t::value_type(v, s));
		}

		m_spectrumId = new int[m_reso.x * m_reso.y * m_reso.z];
		stream->readIntArray(m_spectrumId, m_reso.x * m_reso.y * m_reso.z);
		for (int i = 0; i < m_reso.x*m_reso.y*m_reso.z; ++i) {
			spectrum_map_t::const_iterator iter = m_spectrumMap.find(m_spectrumId[i]);
			if (iter == m_spectrumMap.end()) {
				Log(EError, "Invalid texture spectrum id: %d", m_spectrumId[i]);
			}
		}
	}

	void addChild(const std::string &name, ConfigurableObject *child) {
		if (child->getClass()->derivesFrom(MTS_CLASS(VolumeDataSourceEx))) {
            Assert(m_block == NULL);
			m_block = static_cast<VolumeDataSourceEx*>(child);
		} else
			VolumeDataSource::addChild(name, child);
	}

	Float lookupFloat(const Point &_p) const {
		return m_block->lookupFloat(_p);
	}

    Float lookupFloatEx(uint32_t id, const Point &_p) const {
        return m_block->lookupFloatEx(id, _p);
    }

	Spectrum lookupSpectrum(const Point &_p) const {
        return m_block->lookupSpectrum(_p);
	}

	Spectrum lookupSpectrumEx(uint32_t id, const Point &_p) const {
        return m_block->lookupSpectrumEx(id, _p);
	}

	Vector lookupVector(const Point &_p) const {
        Vector ret = m_block->lookupVector(_p);
        if ( !ret.isZero() ) ret = normalize(ret);
        return ret;
	}

	Vector lookupVectorEx(uint32_t id, const Point &_p) const {
        Vector ret = m_block->lookupVectorEx(id, _p);
        if ( !ret.isZero() ) ret = normalize(ret);
        return ret;
	}

    void lookupBundle(const Point &_p,
        Float *density, Vector *direction, Spectrum *albedo, Float *gloss) const {
        if ( density ) *density = 0.0f;
        if ( direction ) *direction = Vector(0.0f);
        if ( albedo ) *albedo = Spectrum(0.0f);
        if ( gloss ) *gloss = 0.0f;

        m_block->lookupBundle(_p, density, direction, albedo, gloss);
		if (albedo && !albedo->isZero()) updateAlbedo(_p, albedo);
    }
	
    bool supportsBundleLookups() const { return m_block->supportsBundleLookups(); }
	bool supportsFloatLookups() const { return m_block->supportsFloatLookups(); }
    bool supportsSpectrumLookups() const { return m_block->supportsSpectrumLookups(); }
	bool supportsVectorLookups() const { return m_block->supportsVectorLookups(); }
	Float getStepSize() const { return m_stepSize; }
    Float getMaximumFloatValue() const { return m_block->getMaximumFloatValue(); }
    Float getMaximumFloatValueEx(uint32_t id) const { return m_block->getMaximumFloatValueEx(id); }

	MTS_DECLARE_CLASS()

protected:
	inline void updateAlbedo(const Point &_p, Spectrum *albedo) const {
		Point p = m_worldToTexture.transformAffine(_p);
		int tx = floorToInt(p.x), ty = floorToInt(p.y), tz = floorToInt(p.z);

		if (tx < 0 || tx >= m_reso.x || ty < 0 || ty >= m_reso.y || tz < 0 || tz >= m_reso.z)
			return;

		int id = m_spectrumId[(tz*m_reso.y + ty)*m_reso.x + tx];
		spectrum_map_t::const_iterator it = m_spectrumMap.find(id);
		if (it != m_spectrumMap.end())
			*albedo = it->second;
		else
			return;
	}

    ref<VolumeDataSourceEx> m_block;
	Float m_stepSize;

	int *m_spectrumId;
	Vector3i m_reso;

	typedef std::map<int, Spectrum> spectrum_map_t;
	int m_nSpectrumItems;
	spectrum_map_t m_spectrumMap;

	std::string m_textureAlbedoFile;

	Transform m_worldToTexture;
};

MTS_IMPLEMENT_CLASS_S(TextureAlbedoVolume, false, VolumeDataSourceEx);
MTS_EXPORT_PLUGIN(TextureAlbedoVolume, "texture albedo volume data source");
MTS_NAMESPACE_END
