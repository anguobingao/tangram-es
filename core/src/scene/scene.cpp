#include "scene/scene.h"

#include "data/tileSource.h"
#include "gl/framebuffer.h"
#include "gl/shaderProgram.h"
#include "labels/labelManager.h"
#include "scene/dataLayer.h"
#include "scene/importer.h"
#include "scene/light.h"
#include "scene/spriteAtlas.h"
#include "scene/stops.h"
#include "selection/featureSelection.h"
#include "selection/selectionQuery.h"
#include "style/material.h"
#include "style/style.h"
#include "text/fontContext.h"
#include "util/mapProjection.h"
#include "util/util.h"
#include "util/zipArchive.h"
#include "log.h"

#include <algorithm>

namespace Tangram {

static std::atomic<int32_t> s_serial;

//Scene::Scene() : id(s_serial++) {}

Scene::Scene(Platform& _platform, const Url& _url, std::unique_ptr<View> _view)
    : id(s_serial++),
      m_url(_url),
      m_fontContext(std::make_shared<FontContext>(_platform)),
      m_featureSelection(std::make_unique<FeatureSelection>()),
      m_tileWorker(std::make_unique<TileWorker>(_platform, 2)),
      m_tileManager(std::make_unique<TileManager>(_platform, *m_tileWorker)),
      m_labelManager(std::make_unique<LabelManager>()),
      m_view(std::move(_view)) {
    m_pixelScale = m_view->pixelScale();
    m_fontContext->setPixelScale(m_pixelScale);
}

Scene::Scene(Platform& _platform, const std::string& _yaml, const Url& _url, std::unique_ptr<View> _view)
    : id(s_serial++),
      m_url(_url),
      m_yaml(_yaml),
      m_fontContext(std::make_shared<FontContext>(_platform)),
      m_featureSelection(std::make_unique<FeatureSelection>()),
      m_tileWorker(std::make_unique<TileWorker>(_platform, 2)),
      m_tileManager(std::make_unique<TileManager>(_platform, *m_tileWorker)),
      m_labelManager(std::make_unique<LabelManager>()),
      m_view(std::move(_view)) {
     m_pixelScale = m_view->pixelScale();
     m_fontContext->setPixelScale(m_pixelScale);
}

void Scene::copyConfig(const Scene& _other) {

    m_featureSelection.reset(new FeatureSelection());

    m_config = YAML::Clone(_other.m_config);
    m_fontContext = _other.m_fontContext;

    m_url = _other.m_url;
    m_yaml = _other.m_yaml;

    m_globalRefs = _other.m_globalRefs;

    m_zipArchives = _other.m_zipArchives;
}

Scene::~Scene() {
    //m_tileWorker->stop();
}

const Style* Scene::findStyle(const std::string& _name) const {

    for (auto& style : m_styles) {
        if (style->getName() == _name) { return style.get(); }
    }
    return nullptr;

}

Style* Scene::findStyle(const std::string& _name) {

    for (auto& style : m_styles) {
        if (style->getName() == _name) { return style.get(); }
    }
    return nullptr;
}

UrlRequestHandle Scene::startUrlRequest(Platform& platform, Url url, UrlCallback callback) {
    if (url.scheme() == "zip") {
        UrlResponse response;
        // URL for a file in a zip archive, get the encoded source URL.
        auto source = Importer::getArchiveUrlForZipEntry(url);
        // Search for the source URL in our archive map.
        auto it = m_zipArchives.find(source);
        if (it != m_zipArchives.end()) {
            auto& archive = it->second;
            // Found the archive! Now create a response for the request.
            auto zipEntryPath = url.path().substr(1);
            auto entry = archive->findEntry(zipEntryPath);
            if (entry) {
                response.content.resize(entry->uncompressedSize);
                bool success = archive->decompressEntry(entry, response.content.data());
                if (!success) {
                    response.error = "Unable to decompress zip archive file.";
                }
            } else {
                response.error = "Did not find zip archive entry.";
            }
        } else {
            response.error = "Could not find zip archive.";
        }
        callback(std::move(response));
        return 0;
    }

    // For non-zip URLs, send it to the platform.
    return platform.startUrlRequest(url, std::move(callback));
}

void Scene::addZipArchive(Url url, std::shared_ptr<ZipArchive> zipArchive) {
    m_zipArchives.emplace(url, zipArchive);
}

int Scene::addIdForName(const std::string& _name) {
    int id = getIdForName(_name);

    if (id < 0) {
        m_names.push_back(_name);
        return m_names.size() - 1;
    }
    return id;
}

int Scene::getIdForName(const std::string& _name) const {
    auto it = std::find(m_names.begin(), m_names.end(), _name);
    if (it == m_names.end()) {
        return -1;
    }
    return it - m_names.begin();
}

int Scene::addJsFunction(const std::string& _function) {
    for (size_t i = 0; i < m_jsFunctions.size(); i++) {
        if (m_jsFunctions[i] == _function) { return i; }
    }
    m_jsFunctions.push_back(_function);
    return m_jsFunctions.size()-1;
}

const Light* Scene::findLight(const std::string &_name) const {
    for (auto& light : m_lights) {
        if (light->getInstanceName() == _name) { return light.get(); }
    }
    return nullptr;
}

std::shared_ptr<Texture> Scene::getTexture(const std::string& textureName) const {
    auto texIt = m_textures.find(textureName);
    if (texIt == m_textures.end()) {
        return nullptr;
    }
    return texIt->second;
}

std::shared_ptr<TileSource> Scene::getTileSource(int32_t id) {
    auto it = std::find_if(m_tileSources.begin(), m_tileSources.end(),
                           [&](auto& s){ return s->id() == id; });
    if (it != m_tileSources.end()) {
        return *it;
    }
    return nullptr;
}

std::shared_ptr<TileSource> Scene::getTileSource(const std::string& name) {
    auto it = std::find_if(m_tileSources.begin(), m_tileSources.end(),
                           [&](auto& s){ return s->name() == name; });
    if (it != m_tileSources.end()) {
        return *it;
    }
    return nullptr;
}

void Scene::setPixelScale(float _scale) {
    if (m_pixelScale == _scale) { return; }
    LOGD("setPixelScale %f", _scale);

    m_pixelScale = _scale;

    for (auto& style : m_styles) {
        style->setPixelScale(_scale);
    }
    m_fontContext->setPixelScale(_scale);

    // Tiles must be rebuilt to apply the new pixel scale to labels.
    m_tileManager->clearTileSets();

    // Markers must be rebuilt to apply the new pixel scale.
    if (m_markerManager) {
        m_markerManager->rebuildAll();
    }

}

bool Scene::update(const View& _view, float _dt) {

    m_time += _dt;

    bool markersChanged = m_markerManager->update(_view, _dt);

    for (const auto& style : m_styles) {
        style->onBeginUpdate();
    }

    m_tileManager->updateTileSets(_view);

    auto& tiles = m_tileManager->getVisibleTiles();
    auto& markers = m_markerManager->markers();

    if (_view.changedOnLastUpdate() ||
        m_tileManager->hasTileSetChanged() ||
        markersChanged) {

        for (const auto& tile : tiles) {
            tile->update(_dt, _view);
        }
        m_labelManager->updateLabelSet(_view.state(), _dt, *this, tiles, markers, *m_tileManager);
    } else {
        m_labelManager->updateLabels(_view.state(), _dt, m_styles, tiles, markers);
    }

    bool tilesChanged = m_tileManager->hasTileSetChanged();
    bool tilesLoading = m_tileManager->hasLoadingTiles();

    return tilesChanged || tilesLoading;
}

void Scene::renderBeginFrame(RenderState& _rs) {
    for (const auto& style : m_styles) {
        style->onBeginFrame(_rs);
    }
}

bool Scene::render(RenderState& _rs, View& _view) {

    bool drawnAnimatedStyle = false;
    for (const auto& style : m_styles) {

        bool styleDrawn = style->draw(_rs, _view, *this,
                                      m_tileManager->getVisibleTiles(),
                                      m_markerManager->markers());

        drawnAnimatedStyle |= (styleDrawn && style->isAnimated());
    }
    return drawnAnimatedStyle;
}

void Scene::renderSelection(RenderState& _rs, View& _view, FrameBuffer& _selectionBuffer,
                            std::vector<SelectionQuery>& _selectionQueries) {

    for (const auto& style : m_styles) {

        style->drawSelectionFrame(_rs, _view, *this,
                                  m_tileManager->getVisibleTiles(),
                                  m_markerManager->markers());
    }

    std::vector<SelectionColorRead> colorCache;
    // Resolve feature selection queries
    for (const auto& selectionQuery : _selectionQueries) {
        selectionQuery.process(_view, _selectionBuffer,
                               *m_markerManager, *m_tileManager,
                               *m_labelManager, colorCache);
    }

}
}
