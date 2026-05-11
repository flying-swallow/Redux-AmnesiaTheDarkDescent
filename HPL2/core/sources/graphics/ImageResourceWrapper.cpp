/**
* Copyright 2023 Michael Pollind
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "graphics/ImageResourceWrapper.h"

#include "graphics/Image.h"
#include "resources/TextureManager.h"

namespace hpl {

    ImageResourceWrapper::ImageResourceWrapper() = default;

    ImageResourceWrapper::ImageResourceWrapper(cTextureManager* textureManager, Image* image, bool autoDestroy)
        : m_image(image)
        , m_textureManager(textureManager)
        , m_autoDestroyResource(autoDestroy) {
    }

    ImageResourceWrapper::ImageResourceWrapper(ImageResourceWrapper&& other)
        : m_image(other.m_image)
        , m_textureManager(other.m_textureManager)
        , m_autoDestroyResource(other.m_autoDestroyResource) {
        other.m_image = nullptr;
        other.m_textureManager = nullptr;
    }

    ImageResourceWrapper::~ImageResourceWrapper() {
        if (m_image && m_autoDestroyResource && m_textureManager) {
            m_textureManager->Destroy(m_image);
        }
    }

    void ImageResourceWrapper::operator=(ImageResourceWrapper&& other) {
        if (m_image && m_autoDestroyResource && m_textureManager) {
            m_textureManager->Destroy(m_image);
        }
        m_image = other.m_image;
        m_textureManager = other.m_textureManager;
        m_autoDestroyResource = other.m_autoDestroyResource;
        other.m_image = nullptr;
        other.m_textureManager = nullptr;
    }

    void ImageResourceWrapper::SetAutoDestroyResource(bool autoDestroy) {
        m_autoDestroyResource = autoDestroy;
    }

} // namespace hpl
