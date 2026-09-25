// inflate.h
// Implémentation minimale et autonome de DEFLATE (RFC 1951), pensée pour
// des environnements contraints en RAM (RP2040 : 264 KB).
//
// Design :
//  - Pas d'allocation dynamique : tout est sur la pile (tables Huffman de
//    quelques centaines d'octets max).
//  - Lecture des données compressées via un callback "refill" fourni par
//    l'appelant : permet de streamer directement depuis un fichier (FILE*)
//    sans jamais charger le flux compressé entier en mémoire.
//  - Écriture directe dans le buffer de sortie fourni par l'appelant (pas de
//    fenêtre glissante séparée : les back-references DEFLATE (<=32KB)
//    pointent directement dans le buffer de sortie déjà écrit, ce qui est
//    valide puisqu'on écrit tout séquentiellement).
//
// Limitation volontaire : ce n'est pas optimisé pour la vitesse (décodage
// Huffman bit-à-bit, cf. algorithme "canonique" façon puff.c). Sur PC c'est
// largement suffisant pour décompresser des .class de quelques dizaines de
// KB. Si le débit s'avère insuffisant sur RP2040, remplacer par une table de
// décodage rapide (lookup table) sera l'optimisation naturelle.

#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

namespace jme
{

    // Callback de remplissage : doit écrire jusqu'à `maxLen` octets dans `buf`
    // et retourner le nombre d'octets effectivement lus (0 = fin de flux).
    using RefillFn = std::function<size_t(uint8_t *buf, size_t maxLen)>;

    class BitReader
    {
    public:
        explicit BitReader(RefillFn refill) : refill_(std::move(refill)) {}

        int getBit();
        uint32_t getBits(int n);   // lecture LSB-first (valeurs "normales")
        void alignToByte();        // pour les blocs "stored"
        uint8_t readByteAligned(); // suppose alignToByte() déjà appelé
        bool eof() const { return eof_; }

    private:
        RefillFn refill_;
        uint8_t buffer_[512]{};
        size_t bufLen_ = 0;
        size_t bufPos_ = 0;
        uint32_t bitBuf_ = 0;
        int bitCount_ = 0;
        bool eof_ = false;

        uint8_t nextByte();
    };

    // Décompresse un flux DEFLATE complet vers `out` (capacité `outCap`).
    // Retourne le nombre d'octets décompressés, ou 0 en cas d'erreur / dépassement.
    size_t inflateStream(BitReader &br, uint8_t *out, size_t outCap);

} // namespace jme