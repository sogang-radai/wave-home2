from typing import Optional

import torch
import torch.nn as nn
import torch.nn.functional as F

SEQUENCE_LENGTH: int = 60
GESTURE_COUNT: int = 6

class PointNetFrameEncoderV1(nn.Module):
    def __init__(self):
        super(PointNetFrameEncoderV1, self).__init__()

        # MLP1 (n x 5 -> n x 32 -> n x 64)
        self.mlp1 = nn.Sequential(
            nn.Conv1d(in_channels=5, out_channels=32, kernel_size=1),
            nn.ReLU(inplace=True),
            nn.Conv1d(in_channels=32, out_channels=64, kernel_size=1)
        )
        self.bn1 = nn.BatchNorm1d(64)
        self.relu1 = nn.ReLU(inplace=True)

        # MLP2 (n x 64 -> n x 64 -> n x 128 -> n x 256)
        self.mlp2 = nn.Sequential(
            nn.Conv1d(in_channels=64, out_channels=64, kernel_size=1),
            nn.ReLU(inplace=True),
            nn.Conv1d(in_channels=64, out_channels=128, kernel_size=1),
            nn.ReLU(inplace=True),
            nn.Conv1d(in_channels=128, out_channels=256, kernel_size=1)
        )
        self.bn2 = nn.BatchNorm1d(256)
        self.relu2 = nn.ReLU(inplace=True)

    def forward(self, x, mask: Optional[torch.Tensor] = None):
        x = self.mlp1(x)
        x = self.bn1(x)
        x = self.relu1(x)

        x = self.mlp2(x)
        x = self.bn2(x)
        x = self.relu2(x)

        if mask is not None:
            mask_float = mask.unsqueeze(1).float()
            x = x * mask_float + (1.0 - mask_float) * -1e9

        x = torch.max(x, 2)[0]

        return x

# Adds learnable positional encodings to the frame features before feeding into the Transformer
class LearnablePositionalEncoding(nn.Module):
    def __init__(self, d_model: int, max_len: int = 61): # 60 frames + 1 CLS token
        super().__init__()
        self.pos_embedding = nn.Parameter(torch.zeros(1, max_len, d_model))
        nn.init.trunc_normal_(self.pos_embedding, std=0.02)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # x: (B, T, D)
        return x + self.pos_embedding[:, : x.size(1), :]

class TransformerV1(nn.Module):
    def __init__(self,
        input_dim: int = 5, # (x, y, z, doppler, power) - NOTE: This is the raw point dim, but TransformerV1 takes frame_features
        num_classes: int = 6, # gesture classes
        seq_len: int = 60,
        frame_feature_dim: int = 256, # The dimension of features coming from the frame encoder
        transformer_dim: int = 128,
        num_heads: int = 4, # transformer attention heads
        num_layers: int = 1,
        dropout: float = 0.3,
    ):
        super().__init__() # Call super().__init__() first!
        self.seq_len = seq_len

        self.frame_projection = nn.Identity() if frame_feature_dim == transformer_dim else nn.Linear(frame_feature_dim, transformer_dim)
        self.cls_token = nn.Parameter(torch.zeros(1, 1, transformer_dim))
        nn.init.trunc_normal_(self.cls_token, std=0.02)

        self.positional_encoding = LearnablePositionalEncoding(transformer_dim, max_len=seq_len + 1)

        encoder_layer = nn.TransformerEncoderLayer(
            d_model=transformer_dim,
            nhead=num_heads,
            dim_feedforward=transformer_dim * 2,
            dropout=dropout,
            activation="gelu",
            batch_first=True,
            norm_first=True,
        )
        self.transformer = nn.TransformerEncoder(encoder_layer, num_layers=num_layers)

        self.fc = nn.Linear(transformer_dim, num_classes)

    def forward(self, frame_embeddings: torch.Tensor, mask: Optional[torch.Tensor] = None) -> torch.Tensor:
        # frame_embeddings: (B, seq_len, frame_feature_dim) from GestureClassifierV1
        batch_size, seq_len, frame_feature_dim = frame_embeddings.shape

        # Project frame embeddings to transformer_dim
        projected_frame_features = self.frame_projection(frame_embeddings)  # (B, seq_len, transformer_dim)

        # Prepend a learnable [CLS] token and add temporal positional encodings.
        cls_token = self.cls_token.expand(batch_size, -1, -1)  # (B, 1, transformer_dim)
        tokens = torch.cat([cls_token, projected_frame_features], dim=1)  # (B, seq_len + 1, transformer_dim)
        tokens = self.positional_encoding(tokens)  # (B, seq_len + 1, transformer_dim)

        # Transformer captures temporal dependencies across the frames.
        tokens = self.transformer(tokens)  # (B, seq_len + 1, transformer_dim)

        # Classification uses the [CLS] token representation.
        cls_repr = tokens[:, 0, :]  # (B, transformer_dim)
        logits = self.fc(cls_repr)  # (B, num_classes)

        return logits
    
class GestureClassifierV1(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.frame_encoder = PointNetFrameEncoderV1() # V1 or V2
        self.temporal_head = TransformerV1() # CNNV1 or LSTMV1 or TransformerV1

    def encode_frames(self, points: torch.Tensor, mask: Optional[torch.Tensor] = None) -> torch.Tensor:
        # encoder에 mask도 같이 넘겨줍니다.
        return self.frame_encoder(points, mask)

    def classify_embeddings(self, frame_embeddings: torch.Tensor) -> torch.Tensor:
        return self.temporal_head(frame_embeddings)

    def forward(self, points: torch.Tensor, mask: torch.Tensor) -> torch.Tensor:
        batch_size, seq_len, max_points, features = points.shape

        flat_points = points.view(batch_size * seq_len, max_points, features)
        flat_points = flat_points.transpose(1, 2)

        if mask is not None:
            flat_mask = mask.view(batch_size * seq_len, max_points)
        else:
            flat_mask = None

        frame_embeddings = self.encode_frames(flat_points, flat_mask)
        frame_embeddings = frame_embeddings.view(batch_size, seq_len, -1)

        logits = self.classify_embeddings(frame_embeddings)

        return logits