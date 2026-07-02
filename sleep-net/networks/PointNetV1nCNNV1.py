from typing import Optional, Tuple
import torch
import torch.nn as nn

EMBEDDING_DIM: int = 256
SEQUENCE_LENGTH_BED: int = 160
SEQUENCE_LENGTH_TOSS: int = 40
CLASS_COUNT_BED: int = 3 # 0: None, 1: Awake, 2: Asleep
CLASS_COUNT_TOSS: int = 3   # 0: Calm, 1: Slight, 2: Moderate

class PointNetV1(nn.Module):
    def __init__(self):
        super(PointNetV1, self).__init__()

        self.mlp1 = nn.Sequential(
            nn.Conv1d(in_channels=5, out_channels=32, kernel_size=1),
            nn.ReLU(inplace=True),
            nn.Conv1d(in_channels=32, out_channels=64, kernel_size=1)
        )
        self.bn1 = nn.BatchNorm1d(64)
        self.relu1 = nn.ReLU(inplace=True)

        self.mlp2 = nn.Sequential(
            nn.Conv1d(in_channels=64, out_channels=64, kernel_size=1),
            nn.ReLU(inplace=True),
            nn.Conv1d(in_channels=64, out_channels=128, kernel_size=1),
            nn.ReLU(inplace=True),
            nn.Conv1d(in_channels=128, out_channels=EMBEDDING_DIM, kernel_size=1)
        )
        self.bn2 = nn.BatchNorm1d(EMBEDDING_DIM)
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
            x = x * mask_float

        x = torch.max(x, 2)[0]

        return x


class TemporalCNNV1(nn.Module):
    def __init__(self, num_classes: int, input_size=EMBEDDING_DIM, hidden_size=128):
        super(TemporalCNNV1, self).__init__()

        self.conv_net = nn.Sequential(
            nn.Conv1d(in_channels=input_size, out_channels=hidden_size, kernel_size=5, padding=2),
            nn.ReLU(inplace=True),
            nn.Dropout(p=0.3),

            nn.Conv1d(in_channels=hidden_size, out_channels=hidden_size, kernel_size=3, padding=1),
            nn.ReLU(inplace=True),
            nn.Dropout(p=0.3),

            nn.AdaptiveMaxPool1d(1)
        )
        self.fc = nn.Linear(hidden_size, num_classes)

    def forward(self, x):
        x = x.transpose(1, 2)
        cnn_out = self.conv_net(x)
        features = cnn_out.squeeze(-1)
        logits = self.fc(features)
        return logits


class SleepNet(nn.Module):
    # Overall sleep-net: shared PointNet encoder + two sub-nets.
    #   head_status -> bed-net  (occupancy/status)
    #   head_toss   -> toss-net (tossing)
    def __init__(self) -> None:
        super().__init__()
        self.frame_encoder = PointNetV1()
        self.head_status = TemporalCNNV1(num_classes=CLASS_COUNT_BED)
        self.head_toss = TemporalCNNV1(num_classes=CLASS_COUNT_TOSS)

    def encode_frames(self, points: torch.Tensor, mask: Optional[torch.Tensor] = None) -> torch.Tensor:
        return self.frame_encoder(points, mask)

    def classify_embeddings(self, frame_embeddings: torch.Tensor) -> Tuple[torch.Tensor, torch.Tensor]:
        logits_status = self.head_status(frame_embeddings)
        logits_toss = self.head_toss(frame_embeddings)
        return logits_status, logits_toss

    def forward(self, points: torch.Tensor, mask: torch.Tensor) -> Tuple[torch.Tensor, torch.Tensor]:
        batch_size, seq_len, max_points, features = points.shape

        flat_points = points.view(batch_size * seq_len, max_points, features)
        flat_points = flat_points.transpose(1, 2)

        if mask is not None:
            flat_mask = mask.view(batch_size * seq_len, max_points)
        else:
            flat_mask = None

        frame_embeddings = self.encode_frames(flat_points, flat_mask)
        frame_embeddings = frame_embeddings.view(batch_size, seq_len, -1)

        logits_status, logits_toss = self.classify_embeddings(frame_embeddings)

        return logits_status, logits_toss