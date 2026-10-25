//! Typed access to converted stock skater XML values, without engine objects.
use serde::Deserialize;
use std::{collections::BTreeMap, path::Path};

#[derive(Debug, Deserialize)]
pub struct Field {
    #[serde(rename = "type")]
    pub type_name: String,
    pub data: String,
}

#[derive(Debug, Deserialize)]
pub struct Collection {
    #[serde(rename = "class")]
    pub class_name: String,
    pub key: String,
    pub parent: String,
    pub fields: BTreeMap<String, Field>,
    pub source: String,
    pub sha256: String,
}

#[derive(Debug, Deserialize)]
pub struct Collections {
    version: u32,
    collections: Vec<Collection>,
    /// halo-skate: where each collection and field is (`field`), made at the
    /// first look-up: a look-up went through every collection, spelling
    /// each one's numeric name, thousands of times as a session loaded.
    #[serde(skip)]
    index: std::sync::OnceLock<Index>,
}

/// Each collection by its class and key, and each one's fields by name, as
/// numeric identities (`attrib_hash::numeric_key`): the first of each in
/// order, as the look-up's search found it.
#[derive(Debug, Default)]
struct Index {
    collections: std::collections::HashMap<(u64, u64), usize>,
    fields: Vec<std::collections::HashMap<u64, String>>,
}

impl Index {
    fn of(collections: &[Collection]) -> Self {
        use crate::attrib_hash::numeric_key;
        let mut index = Index::default();
        for (i, item) in collections.iter().enumerate() {
            index.collections.entry((numeric_key(&item.class_name), numeric_key(&item.key))).or_insert(i);
            let mut fields = std::collections::HashMap::new();
            for key in item.fields.keys() {
                fields.entry(numeric_key(key)).or_insert_with(|| key.clone());
            }
            index.fields.push(fields);
        }
        index
    }
}

impl Collections {
    pub fn entries(&self) -> &[Collection] {
        &self.collections
    }

    pub fn load(asset_root: &Path) -> Result<Self, String> {
        let path = asset_root.join("private/stock/skater-collections.json");
        let bytes = std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
        let data: Self = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
        if data.version != 1 {
            return Err(format!(
                "Unsupported skater collections version {}",
                data.version
            ));
        }
        let mut identities = std::collections::BTreeSet::new();
        for item in &data.collections {
            if !identities.insert((crate::attrib_hash::numeric_key(&item.class_name), crate::attrib_hash::numeric_key(&item.key))) {
                return Err(format!(
                    "Duplicate collection {}/{}",
                    item.class_name, item.key
                ));
            }
        }
        Ok(data)
    }

    pub fn field(&self, class: &str, key: &str, name: &str) -> Result<&Field, String> {
        // Converted vaults can retain numeric identities when debug names were
        // unavailable. Both spellings identify the same native AttribSys entry.
        let index = self.index.get_or_init(|| Index::of(&self.collections));
        let class_id = crate::attrib_hash::numeric_key(class);
        let field_id = crate::attrib_hash::numeric_key(name);
        let mut current = key;
        for _ in 0..=self.collections.len() {
            let current_id = crate::attrib_hash::numeric_key(current);
            let at = *index
                .collections
                .get(&(class_id, current_id))
                .ok_or_else(|| format!("Missing stock collection {class}/{current}"))?;
            let item = &self.collections[at];
            if let Some(field) = item.fields.get(name).or_else(|| {
                index.fields[at].get(&field_id).and_then(|key| item.fields.get(key))
            }) {
                return Ok(field);
            }
            if item.parent.is_empty() {
                return Err(format!("Missing stock field {class}/{key}/{name}"));
            }
            current = &item.parent;
        }
        Err(format!("Cyclic stock collection inheritance {class}/{key}"))
    }

    pub fn float(&self, class: &str, key: &str, name: &str) -> Result<f32, String> {
        let field = self.field(class, key, name)?;
        if field.type_name != "EA::Reflection::Float" {
            return Err(format!("Expected float at {class}/{key}/{name}"));
        }
        let words = decode_words::<1>(&field.data)?;
        let value = f32::from_bits(words[0]);
        if !value.is_finite() {
            return Err(format!("Non-finite stock float {class}/{key}/{name}"));
        }
        Ok(value)
    }

    pub fn integer(&self, class: &str, key: &str, name: &str) -> Result<u32, String> {
        let field = self.field(class, key, name)?;
        if !matches!(
            field.type_name.as_str(),
            "EA::Reflection::Int32" | "EA::Reflection::UInt32"
        ) {
            return Err(format!("Expected integer at {class}/{key}/{name}"));
        }
        Ok(decode_words::<1>(&field.data)?[0])
    }

    pub fn boolean(&self, class: &str, key: &str, name: &str) -> Result<bool, String> {
        let field = self.field(class, key, name)?;
        if field.type_name != "EA::Reflection::Bool" {
            return Err(format!("Expected boolean at {class}/{key}/{name}"));
        }
        // Attributes store the byte followed by padding; layout bools may
        // contain only the byte. Reading a big-endian u32 gives the wrong bit.
        match field.data.get(..2) {
            Some("00") => Ok(false),
            Some("01") => Ok(true),
            _ => Err(format!("Invalid stock boolean {class}/{key}/{name}")),
        }
    }

    pub fn words<const N: usize>(
        &self,
        class: &str,
        key: &str,
        name: &str,
    ) -> Result<[u32; N], String> {
        decode_words(&self.field(class, key, name)?.data)
    }
}

fn decode_words<const N: usize>(text: &str) -> Result<[u32; N], String> {
    let hex: String = text.chars().filter(|c| !c.is_whitespace()).collect();
    if hex.len() != N * 8 || !hex.is_ascii() {
        return Err(format!(
            "Expected {N} big-endian words, found {} bytes of hex",
            hex.len()
        ));
    }
    let mut words = [0; N];
    for (i, word) in words.iter_mut().enumerate() {
        *word = u32::from_str_radix(&hex[i * 8..i * 8 + 8], 16)
            .map_err(|e| format!("Invalid collection payload: {e}"))?;
    }
    Ok(words)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numeric_export_names_resolve_readable_manual_settings() {
        // Identities and payload from the prepared vault; no asset IO or gameplay.
        let data: Collections = serde_json::from_value(serde_json::json!({
            "version": 1, "collections": [{
                "class": "anim_motion", "key": crate::attrib_hash::numeric_name("manual"),
                "parent": "", "source": "fixture", "sha256": "",
                "fields": { "Hash_A353CE1670D3AA40": { "type": "EA::Reflection::Float", "data": "3D23D70A" } }
            }]
        })).unwrap();
        assert_eq!(data.float("anim_motion", "manual", "manual_clamp_vel").unwrap().to_bits(), 0x3d23d70a);
        assert!(data.field("anim_motion", "manual", "missing").is_err());
    }

    #[test]
    fn readable_names_resolve_numeric_queries_through_inheritance() {
        let data: Collections = serde_json::from_value(serde_json::json!({
            "version": 1, "collections": [
                {"class":"example", "key":"child", "parent":crate::attrib_hash::numeric_name("base"),
                 "source":"fixture", "sha256":"", "fields":{}},
                {"class":"example", "key":"base", "parent":"", "source":"fixture", "sha256":"",
                 "fields":{"value":{"type":"EA::Reflection::Float", "data":"3F800000"}}}
            ]
        })).unwrap();
        assert_eq!(data.float(&crate::attrib_hash::numeric_name("example"),
            &crate::attrib_hash::numeric_name("child"), &crate::attrib_hash::numeric_name("value")).unwrap(), 1.0);
    }
}
